"""Execute production region resolution and partial clears with instrumented Vulkan commands."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", required=True, type=Path)
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_xfer.cpp")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    texture = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    fixture = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
struct Texture {
    VkImage image=(VkImage)1; VkImageType type=VK_IMAGE_TYPE_2D; VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    uint64_t gpu=0x1000;
    uint32_t width=64,height=32,depth=1,layers=4,levels=4,skip=0;
    int nvn_format=1; bool bc=false,astc=false,sparse=false,gpu_written=false,released=false,only_cleared=false;
} tex;
struct nvn_tex { uint64_t gpu=0x1000; };
struct nvn_view { bool set_levels=false,set_layers=false; uint8_t base_level=0,nlevels=0; uint16_t min_layer=0,nlayers=0; };
struct NVNcopyRegion { int32_t xoffset=0,yoffset=0,zoffset=0,width=0,height=0,depth=1; };
struct NvnState {};
union Color { float f[4]; uint32_t u[4]; };
struct nvn_p_clear_tex { const nvn_tex* tex; nvn_view view{}; bool has_view=true; NVNcopyRegion r{}; uint32_t mask=15; Color c{}; };
struct TextureAttachment { VkImageView view; VkFormat format; VkImage blocks; };
unsigned selected_mip, expected_mip, first_layer, clear_count, whole_count, touched[4][4];
unsigned block_copies;
void warn(int, const char*) {}
template<typename... T> void logf(const char*,T...) {}
Texture* texture_get(const nvn_tex*,bool) { return &tex; }
int view_nvn_format(const nvn_tex*,const nvn_view*) { return 1; }
int nvn_block_size(int,int* w,int* h) {
    const bool raw=tex.format==VK_FORMAT_BC4_UNORM_BLOCK;
    *w=*h=raw ? 4 : 1;
    return raw ? 8 : 4;
}
void texture_resident(Texture*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) {}
void end_pass(VkCommandBuffer) {}
void full_barrier(VkCommandBuffer) {}
struct GprofSpan { GprofSpan(VkCommandBuffer,const char*) {} };
void texture_written(Texture* t) { t->gpu_written=true; }
''' + "\n".join(block(texture, m) for m in ("unsigned view_level(", "unsigned image_level(", "unsigned view_layer(")) + r'''
TextureAttachment texture_attachment(Texture* t,const nvn_tex*,const nvn_view* view) {
    selected_mip=image_level(t,view_level(view)); first_layer=view_layer(view);
    CHECK(view->nlevels==1 && view->nlayers==1);
    const bool raw=t->format==VK_FORMAT_BC4_UNORM_BLOCK;
    return {(VkImageView)1,t->format,raw ? (VkImage)2 : VK_NULL_HANDLE};
}
void vkCmdCopyImage(VkCommandBuffer,VkImage src,VkImageLayout src_layout,VkImage dst,VkImageLayout dst_layout,
                    uint32_t count,const VkImageCopy* region) {
    const bool load=block_copies%2==0;
    const uint32_t slice=2+block_copies/2;
    CHECK(src_layout==VK_IMAGE_LAYOUT_GENERAL && dst_layout==VK_IMAGE_LAYOUT_GENERAL && count==1);
    const VkImage compressed=(VkImage)1, blocks=(VkImage)2;
    CHECK(src==(load ? compressed : blocks) && dst==(load ? blocks : compressed));
    CHECK(region->srcSubresource.mipLevel==(load ? 1u : 0u) && region->dstSubresource.mipLevel==(load ? 0u : 1u));
    CHECK(region->srcSubresource.baseArrayLayer==0 && region->dstSubresource.baseArrayLayer==0);
    CHECK(region->srcSubresource.layerCount==1 && region->dstSubresource.layerCount==1);
    CHECK(region->srcOffset.z==int32_t(slice) && region->dstOffset.z==int32_t(slice));
    CHECK(region->extent.width==(load ? 63u : 16u) && region->extent.height==(load ? 31u : 8u) && region->extent.depth==1);
    ++block_copies;
}
void vkCmdBeginRendering(VkCommandBuffer,const VkRenderingInfo* info) {
    if (selected_mip != expected_mip) std::fprintf(stderr,"selected mip %u instead of %u\n",selected_mip,expected_mip);
    CHECK(selected_mip==expected_mip);
    if (tex.format==VK_FORMAT_BC4_UNORM_BLOCK) {
        CHECK(block_copies==2*clear_count+1);
        CHECK(info->renderArea.extent.width==16 && info->renderArea.extent.height==8);
    } else {
        CHECK(info->renderArea.extent.width==std::max(1u,tex.width>>selected_mip));
        CHECK(info->renderArea.extent.height==std::max(1u,tex.height>>selected_mip));
    }
}
void vkCmdClearAttachments(VkCommandBuffer,uint32_t,const VkClearAttachment*,uint32_t,const VkClearRect*) {
    ++clear_count; ++touched[selected_mip][first_layer];
}
void vkCmdEndRendering(VkCommandBuffer) {}
void vkCmdClearColorImage(VkCommandBuffer,VkImage,VkImageLayout,const VkClearColorValue*,uint32_t,const VkImageSubresourceRange* r) {
    CHECK(r->baseMipLevel==expected_mip); ++whole_count;
}
void vkCmdClearDepthStencilImage(VkCommandBuffer,VkImage,VkImageLayout,const VkClearDepthStencilValue*,uint32_t,const VkImageSubresourceRange*) { CHECK(false); }
''' + block(texture, "void copy_block_attachment(") + "\n" + block(source, "struct Sub {") + ";\n" + block(source, "bool resolve(") + "\n" + block(source, "void clear_tex(") + r'''
void run(uint32_t skip,uint32_t logical,uint32_t layer,bool whole) {
    tex=Texture{}; tex.skip=skip; std::memset(touched,0,sizeof touched); clear_count=whole_count=0;
    expected_mip=logical-skip;
    nvn_tex input; nvn_p_clear_tex p{&input}; p.view.set_levels=true; p.view.base_level=(uint8_t)logical; p.view.nlevels=1;
    p.view.set_layers=true; p.view.min_layer=(uint16_t)layer; p.view.nlayers=2;
    p.r.xoffset=whole ? 0 : 1; p.r.yoffset=whole ? 0 : 1;
    p.r.width=whole ? (int32_t)(tex.width>>expected_mip) : 2;
    p.r.height=whole ? (int32_t)(tex.height>>expected_mip) : 2; p.r.depth=2;
    NvnState state; clear_tex(VK_NULL_HANDLE,state,p);
    CHECK(tex.gpu_written);
    CHECK(whole ? whole_count==1 && clear_count==0 : whole_count==0 && clear_count==2);
    if (!whole) for (unsigned mip=0;mip<4;++mip) for(unsigned l=0;l<4;++l)
        CHECK(touched[mip][l]==unsigned(mip==expected_mip && l>=layer && l<layer+2));
}
int main() {
    run(1,2,1,false); run(2,3,1,false); run(1,1,0,false);
    run(0,2,1,false); run(0,0,0,false); run(1,2,1,true);
    tex=Texture{};
    tex.type=VK_IMAGE_TYPE_3D; tex.format=VK_FORMAT_BC4_UNORM_BLOCK;
    tex.width=126; tex.height=62; tex.depth=8; tex.layers=1;
    expected_mip=1;
    clear_count=whole_count=block_copies=0;
    std::memset(touched,0,sizeof touched);
    nvn_tex input;
    nvn_p_clear_tex clear{&input};
    clear.view.set_levels=true; clear.view.base_level=1; clear.view.nlevels=1;
    clear.r={4,4,2,3,3,2};
    NvnState state;
    clear_tex(VK_NULL_HANDLE,state,clear);
    CHECK(tex.gpu_written && clear_count==2 && whole_count==0 && block_copies==4);
    CHECK(touched[1][0]==0 && touched[1][1]==0 && touched[1][2]==1 && touched[1][3]==1);
    std::puts("PASS: array clears and BC volume clears preserve mip and slice addressing");
}
'''
    with tempfile.TemporaryDirectory(prefix="hookshot-clear-mip-") as directory:
        cpp, exe = Path(directory) / "test.cpp", Path(directory) / "test.exe"
        cpp.write_text(fixture, encoding="utf-8")
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if args.sanitize else []
        subprocess.run([args.cxx,"-std=c++20","-O1","-Wall","-Werror",
                        "-Wno-missing-field-initializers",*flags,
                        "-I"+str(args.headers),str(cpp),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == "__main__":
    main()
