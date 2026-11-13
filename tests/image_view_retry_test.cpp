#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fixture {
void require(bool ok, const char* why) { if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
/* FORMATS */
enum { NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A,
       NVN_TEXTURE_SWIZZLE_ZERO, NVN_TEXTURE_SWIZZLE_ONE };
bool fail_commit = false;
struct ViewMap : std::unordered_map<uint64_t, VkImageView> {
    auto emplace(uint64_t key, VkImageView view) {
        if (fail_commit) { fail_commit = false; throw std::bad_alloc(); }
        return std::unordered_map<uint64_t, VkImageView>::emplace(key, view);
    }
};
struct Texture {
    VkImage image = reinterpret_cast<VkImage>(uintptr_t(1));
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM; VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width=64, height=64, levels=4, skip=0, layers=2, depth=8, id=1;
    uint64_t gpu=0x1000, view_failed_frame=0; bool astc=false, bc=false, cube=false;
    ViewMap views;
};
struct nvn_texdesc { unsigned swizzle_r=0, swizzle_g=1, swizzle_b=2, swizzle_a=3; int format=42; };
struct nvn_tex { nvn_texdesc d; };
struct nvn_view {
    bool set_levels=false, set_layers=false, set_swizzle=false;
    uint32_t base_level=0, nlevels=0, min_layer=0, nlayers=0; unsigned swz[4]{0,1,2,3};
};
enum class ViewUse { Sample, Attachment, Storage };
struct { VkDevice device{}; bool view_2d_of_3d=true; } dev;
uint64_t frame_no=1;
uint64_t mix(uint64_t a, uint64_t b) { return (a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2))); }
int view_nvn_format(const nvn_tex* t, const nvn_view*) { return t->d.format; }
struct Format { VkFormat format; };
Format vk_format(int) { return {VK_FORMAT_R8G8B8A8_UNORM}; }
VkFormat linear_twin(VkFormat f) { return f; }
VkFormatProperties format_props(VkFormat) { VkFormatProperties p{}; p.optimalTilingFeatures=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT; return p; }
bool is_bc(VkFormat f) { return f == VK_FORMAT_BC3_UNORM_BLOCK || f == VK_FORMAT_BC3_SRGB_BLOCK; }
bool is_depth_format(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT; }
bool is_astc(VkFormat f) { return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }
void logf(const char*, ...) {}
// Diagnostic toggles are outside this allocation/cache fixture; do not inherit the runner's environment.
const char* getenv(const char*) { return nullptr; }
bool env_on(const char* name) { const char* v = getenv(name); return v && !env_value_off(v); }   // (the switches through the stand-in)
std::vector<VkResult> results;
std::vector<VkImageViewCreateInfo> creates;
std::set<VkImageView> live;
unsigned next_handle=10, destroyed=0;
VkResult vkCreateImageView(VkDevice, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks*, VkImageView* out) {
    creates.push_back(*ci);
    VkResult result=VK_SUCCESS;
    if (!results.empty()) { result=results.front(); results.erase(results.begin()); }
    if (result != VK_SUCCESS) { *out=reinterpret_cast<VkImageView>(uintptr_t(0xdead)); return result; }
    *out=reinterpret_cast<VkImageView>(uintptr_t(next_handle++)); live.insert(*out); return VK_SUCCESS;
}
void vkDestroyImageView(VkDevice, VkImageView v, const VkAllocationCallbacks*) {
    require(live.erase(v) == 1, "destroy only a newly owned live view, exactly once"); destroyed++;
}
/* ACTUAL */

VkImageView route(Texture& tex, const std::string& name, uint32_t level=0) {
    if (name == "astc-all") return astc_target_view(&tex, level);
    if (name == "astc-layer") return astc_level_view(&tex, level, 1);
    if (name == "unswizzle") {
        if (!unswizzle_view(tex, level, VK_FORMAT_R8G8B8A8_UNORM)) return VK_NULL_HANDLE;
        return tex.views.at(mix(0x5B1Cu, mix(level, (uint64_t)VK_FORMAT_R8G8B8A8_UNORM)));
    }
    nvn_tex input; nvn_view v; v.set_levels=true; v.base_level=level; v.nlevels=1;
    const auto use=name=="sample" ? ViewUse::Sample : name=="storage" ? ViewUse::Storage : ViewUse::Attachment;
    return texture_view(&tex,&input,&v,VK_IMAGE_VIEW_TYPE_2D_ARRAY,use,nullptr,VK_FORMAT_UNDEFINED);
}
void verify_cache(const Texture& t) {
    for (auto [key,v] : t.views) { (void)key; require(!v || live.count(v), "cache owns only live handles"); }
}
int run(const std::string& name,const std::string& test) {
    Texture t;
    if (name == "factory") {
        VkResult result=VK_SUCCESS;
        t.format=VK_FORMAT_BC3_UNORM_BLOCK;
        uint32_t mips=1, layers=2;
        if (test=="fallback-success") results={VK_ERROR_FORMAT_NOT_SUPPORTED,VK_SUCCESS};
        else if (test=="fallback-oom") results={VK_ERROR_FORMAT_NOT_SUPPORTED,VK_ERROR_OUT_OF_DEVICE_MEMORY};
        else if (test=="fallback-mixed") results={VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_FORMAT_NOT_SUPPORTED};
        else if (test=="invalid-usage") t.usage=0;
        else if (test=="invalid-block-mips") mips=2;
        else if (test=="subresources") { t.format=VK_FORMAT_R8G8B8A8_UNORM; mips=2; layers=1; }
        else require(false,"known factory case");
        const VkComponentMapping cm{VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
        auto v=make_view(&t,VK_IMAGE_VIEW_TYPE_2D_ARRAY,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,1,mips,0,layers,cm,VK_IMAGE_USAGE_STORAGE_BIT,&result);
        if (test=="fallback-success") {
            require(v && result==VK_SUCCESS && creates.size()==2,"BC single-layer fallback succeeds");
            require(creates.back().viewType==VK_IMAGE_VIEW_TYPE_2D && creates.back().subresourceRange.layerCount==1,"fallback view shape preserved");
        } else if (test=="fallback-oom") require(!v && result==VK_ERROR_OUT_OF_DEVICE_MEMORY,"fallback OOM remains transient");
        else if (test=="fallback-mixed") require(!v && result==VK_ERROR_OUT_OF_HOST_MEMORY,"earlier transient error survives failed compatibility fallback");
        else if (test=="subresources") {
            require(v && creates.size()==1 && creates.back().subresourceRange.baseMipLevel==1 && creates.back().subresourceRange.levelCount==2 && creates.back().components.r==VK_COMPONENT_SWIZZLE_B,"view description preserved");
        } else require(!v && result==VK_ERROR_FORMAT_NOT_SUPPORTED && creates.empty(),"invalid view rejects before Vulkan");
        return 0;
    }
    if (test=="healthy") {
        auto a=route(t,name); auto b=route(t,name); require(a && a==b && creates.size()==1,"healthy cached handle reused");
    } else if (test=="host-oom" || test=="device-oom") {
        results={test=="host-oom" ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY};
        require(!route(t,name) && t.views.empty(),"transient failure is not permanent cache entry");
        require(!route(t,name) && !route(t,name,1) && creates.size()==1,"uncached views throttled within failed frame");
        frame_no++;
        auto a=route(t,name); require(a && creates.size()==2 && route(t,name)==a,"retry succeeds next frame");
    } else if (test=="permanent") {
        results={VK_ERROR_FORMAT_NOT_SUPPORTED}; require(!route(t,name),"unsupported view fails"); frame_no++;
        require(!route(t,name) && creates.size()==1,"permanent negative cache remains bounded");
    } else if (test=="commit-failure") {
        fail_commit=true; require(!route(t,name) && t.views.empty() && live.empty() && destroyed==1,"uncommitted view rolled back");
        require(!route(t,name) && creates.size()==1,"failed commit throttled without allocation");frame_no++;
        require(route(t,name) && creates.size()==2,"commit failure retry succeeds");
    } else if (test=="negative-commit-failure") {
        results={VK_ERROR_FORMAT_NOT_SUPPORTED}; fail_commit=true;
        require(!route(t,name) && t.views.empty() && live.empty() && destroyed==0,"failed negative-cache commit owns no view");
        require(!route(t,name) && creates.size()==1,"negative-cache commit failure throttled"); frame_no++;
        require(route(t,name) && creates.size()==2,"negative-cache commit retry succeeds");
    } else if (test=="independent") {
        auto a=route(t,name); results={VK_ERROR_OUT_OF_DEVICE_MEMORY};
        require(a && !route(t,name,1) && route(t,name)==a && live.count(a),"existing cached handle survives another key failure");
        Texture other; require(route(other,name) && creates.size()==3,"other texture is not throttled");
    } else require(false,"known cache case");
    verify_cache(t); return 0;
}
} // namespace fixture
int main(int argc,char** argv) { fixture::require(argc==3,"route/case arguments");return fixture::run(argv[1],argv[2]); }
