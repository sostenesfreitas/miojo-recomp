"""The overlays' render pass over the window's image (gpu/vk/vk_imgui.cpp ui::record): the textures copied in first and taken
from the backend (its own upload waits on the queue), then the image from TRANSFER_DST to COLOR_ATTACHMENT after the
transfers that wrote it (the game's picture, its scaling, frame generation), one render pass that loads and stores it, and
back to TRANSFER_DST after the attachment writes, for the transfers and the present that follow. Nothing recorded for an
image with no view.

python -I -B tests/test_ui_record.py --cxx g++ --headers path/to/Vulkan-Headers/include
Executes the real record() and layout() with recording stubs; no GPU.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, default=None)
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_imgui.cpp").read_text(encoding="utf-8")
    fixture = r'''#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum { ImTextureStatus_OK, ImTextureStatus_Destroyed, ImTextureStatus_WantCreate };
bool can_make = true;
void logf(const char*, ...) {}
struct ImTextureData { int Status; };
struct ImDrawData { std::vector<ImTextureData*>* Textures; };
std::string calls;
ImDrawData draw_data;
namespace ImGui { void Render() { calls += "R"; } ImDrawData* GetDrawData() { return &draw_data; } }
void ImGui_ImplVulkan_RenderDrawData(ImDrawData* dd, VkCommandBuffer) { CHECK(dd->Textures == nullptr); calls += "D"; }
struct { std::vector<VkImageView> swap_views; std::vector<VkImage> swap_images; bool hdr = false; } dev;
int64_t presents = 0;
bool srgb_target = false;
void reap(bool) { calls += "r"; }
void linearize(ImDrawData*) { calls += "L"; }
void hdr10(ImDrawData*) { calls += "H"; }
void update_texture(VkCommandBuffer, ImTextureData* t) { calls += "U"; if (can_make) t->Status = ImTextureStatus_OK; }
std::vector<VkImageMemoryBarrier2> barriers;
void vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo* d) {
    CHECK(d->imageMemoryBarrierCount == 1 && !d->bufferMemoryBarrierCount && !d->memoryBarrierCount);
    barriers.push_back(d->pImageMemoryBarriers[0]);
    calls += "B";
}
VkRenderingInfo rendering;
VkRenderingAttachmentInfo attachment;
void vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo* ri) {
    rendering = *ri;
    CHECK(ri->colorAttachmentCount == 1);
    attachment = ri->pColorAttachments[0];
    calls += "[";
}
void vkCmdEndRendering(VkCommandBuffer) { calls += "]"; }
''' + block(src, "void layout(VkCommandBuffer cmd,") + "\n" + block(src, "void record_to(VkCommandBuffer cmd,") + "\n" + block(src, "void record(VkCommandBuffer cmd,") + r'''
int main() {
    VkCommandBuffer cmd = (VkCommandBuffer)0x10;
    dev.swap_images = {(VkImage)0x100, (VkImage)0x101, (VkImage)0x102};
    dev.swap_views = {(VkImageView)0x200, (VkImageView)0x201, VK_NULL_HANDLE};
    ImTextureData atlas{ImTextureStatus_WantCreate}, logo{ImTextureStatus_OK};
    std::vector<ImTextureData*> textures = {&atlas, &logo};
    draw_data.Textures = &textures;
    record(cmd, 1, 640, 360);
    CHECK(calls == "RrUB[D]B");   // (render, reap, the new texture, to the attachment, the pass, back)
    CHECK(presents == 1);
    const VkImageMemoryBarrier2& in = barriers[0];
    CHECK(in.image == (VkImage)0x101);
    CHECK(in.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && in.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(in.srcStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT && in.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(in.dstStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK((in.dstAccessMask & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) && (in.dstAccessMask & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
    CHECK(in.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && in.subresourceRange.levelCount == 1 &&
          in.subresourceRange.layerCount == 1);
    CHECK(attachment.imageView == (VkImageView)0x201 && attachment.imageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
    CHECK(rendering.renderArea.offset.x == 0 && rendering.renderArea.offset.y == 0 && rendering.renderArea.extent.width == 640 &&
          rendering.renderArea.extent.height == 360 && rendering.layerCount == 1);
    const VkImageMemoryBarrier2& out = barriers[1];
    CHECK(out.image == (VkImage)0x101);
    CHECK(out.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && out.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(out.srcStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT && out.srcAccessMask == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(out.dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT && (out.dstAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT));
    // (an sRGB image without a UNORM view: the colours made linear before the pass)
    calls.clear(), barriers.clear(), srgb_target = true, draw_data.Textures = &textures;
    record(cmd, 0, 640, 360);
    CHECK(calls == "RrLB[D]B" && barriers[0].image == (VkImage)0x100);
    // (an HDR10 image: the colours as HDR10 shows them, at the paper white)
    calls.clear(), barriers.clear(), srgb_target = false, dev.hdr = true, draw_data.Textures = &textures;
    record(cmd, 0, 640, 360);
    CHECK(calls == "RrHB[D]B");
    dev.hdr = false;
    // (an image with no view, or past the views: nothing recorded on it)
    calls.clear(), barriers.clear(), srgb_target = false, draw_data.Textures = &textures;
    record(cmd, 2, 640, 360);
    record(cmd, 7, 640, 360);
    CHECK(calls == "RrRr" && barriers.empty());
    // (a texture that could not be made: nothing drawn that frame, no barrier, no pass; made the next frame, all as before)
    ImTextureData failed{ImTextureStatus_WantCreate}, gone{ImTextureStatus_Destroyed};
    std::vector<ImTextureData*> more = {&failed, &gone};
    calls.clear(), barriers.clear(), can_make = false, draw_data.Textures = &more;
    record(cmd, 0, 640, 360);
    CHECK(calls == "RrU" && barriers.empty() && draw_data.Textures == nullptr);
    calls.clear(), can_make = true, draw_data.Textures = &more;
    record(cmd, 0, 640, 360);
    CHECK(calls == "RrUB[D]B");
    std::printf("PASS: the overlays' pass: textures, barrier in, load and store, barrier out, nothing while a texture is missing\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="ui-record-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        cmd = [a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
        if a.headers:
            cmd.append("-I" + str(a.headers))
        subprocess.run(cmd + [str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
