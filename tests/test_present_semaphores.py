"""Compile actual swapchain lifecycle/selection statements with CPU Vulkan stubs.
Usage: python -I -B tests/test_present_semaphores.py [--cxx clang++]
Checks per-image ownership and recovery from recreation failures. No GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")
RUN_ALL_ARGS = ["--cxx", "{cxx}"]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
p.add_argument('--cxx', default='clang++')
a = p.parse_args()
source = a.source.resolve()
main = (source/'gpu/vk/vk_main.cpp').read_text()
replay = (source/'gpu/vk/vk_replay.cpp').read_text()
start = replay.index('void present_image(')
# Read the complete top-level function; capture dependencies can grow its text.
present = replay[start:replay.index('\n}', start) + 2]
# (present ahead, #1702: the present itself is present_image's end, present_out, which present_commit also runs; its wait
# is read from there, the complete function as above)
out_start = replay.index('static bool present_out(uint32_t index, std::unique_lock<std::mutex>& qk) {')
present_out = replay[out_start:replay.index('\n}', out_start) + 2]
signal = re.search(r'    signal\.semaphore = [^;]+;', present)[0]
wait = re.search(r'    pi\.pWaitSemaphores = [^;]+;', present_out)[0]
acquire_wait = re.search(r'    waits\[0\]\.semaphore = [^;]+;', present)[0]
assert 'acquire_timeout, f.acquired, VK_NULL_HANDLE, &index)' in present
# Include the actual creation attempt: passing oldSwapchain retires it even on failure.
start = main.index('    ci.oldSwapchain = dev.swapchain;')
end = main.index('    logf("vk: swapchain', start)
tail = main[start:end]
retire_start = main.index('void retire_swapchain()')
retire = main[retire_start:main.index('\n}', retire_start) + 2]
loss_start = main.index('bool invalidate_lost_surface(')
loss = main[loss_start:main.index('\n}', loss_start) + 2]
acquire_loss_start = present.index('    if (res == VK_ERROR_SURFACE_LOST_KHR)')
acquire_loss = present[acquire_loss_start:present.index('\n#ifndef _WIN32', acquire_loss_start)]
present_loss_start = present_out.index('    if (res == VK_ERROR_SURFACE_LOST_KHR)')
present_loss = present_out[present_loss_start:present_out.index('\n    present_finish(res, false);', present_loss_start)]   # (the frame log and the failure line follow, in present_finish)
commit_start = replay.index('bool present_commit(const PendingPresent& p)')
commit = replay[commit_start:replay.index('\n}', commit_start) + 2]
begin = present[present.index('{') + 1:present.index('    if (src) {')]
reflex = (source/'gpu/vk/vk_reflex.cpp').read_text()
reset_start = reflex.index('void reflex_swapchain_destroyed(') if 'void reflex_swapchain_destroyed(' in reflex else -1
reset = reflex[reset_start:reflex.index('\n}', reset_start)+2] if reset_start >= 0 else 'void reflex_swapchain_destroyed() {}'
fixture = r'''
#include <vector>
#include <set>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <atomic>
#include <mutex>
using VkSemaphore=uint64_t; using VkImage=uint64_t; using VkSwapchainKHR=uint64_t; using VkResult=int;
constexpr uint64_t VK_NULL_HANDLE=0;
constexpr int VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO=1, VK_SUCCESS=0;
constexpr VkResult VK_ERROR_SURFACE_LOST_KHR = -1000000000;
constexpr int VK_PRESENT_MODE_MAILBOX_KHR=1, VK_PRESENT_MODE_IMMEDIATE_KHR=2;
struct VkSemaphoreCreateInfo { int type; };
struct Extent { uint32_t width,height; };
struct Device { int device=1;uint64_t swapchain=0;int swap_format=0;Extent swap_extent{};bool swap_mutable=false;std::atomic<uint32_t> swap_spare{0};
    std::vector<VkImage> swap_images;std::vector<VkSemaphore> swap_rendered;
    std::atomic<uint64_t> swap_resize{0};std::mutex present_mu;uint64_t swapchain_generation=0;
    int instance=1;uint64_t surface=1;bool present_queue_supported=true; } dev;
struct { int hdr=0; } opts;   // (the Insert menu's HDR row: present_image remakes the swapchain when it changes)
unsigned image_count=4, made=0, fail_at=0;uint64_t serial=1000;
int query_result=0,data_result=0;uint64_t destroyed_swapchain=0;
bool idle=false, bad_destroy=false; std::set<uint64_t> live,used;
std::set<uint64_t> chains, retired;
unsigned surfaces_destroyed=0;
uint64_t next_chain=0;int create_result=0;bool bad_chain=false;
struct CreateInfo {uint64_t oldSwapchain=0;unsigned imageUsage=1,minImageCount=4,flags=0;const void* pNext=nullptr;};
// (hookshot: the overlays' mutable sRGB swapchain, vk_main.cpp: its format list, never used by these formats)
using VkFormat=int;
constexpr int VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO=2, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT=16,
    VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR=4;
struct VkImageFormatListCreateInfo {int sType;const void* pNext=nullptr;uint32_t viewFormatCount=0;const VkFormat* pViewFormats=nullptr;};
VkFormat unorm_of(VkFormat f) {return f;}
void reflex_swapchain_info(CreateInfo&) {}
struct Reflex {uint64_t swapchain=0,swapchain_generation=0;bool latency=false,frame_marked=false;int mode=-1;std::mutex mu;} cached;
Reflex& rx(){return cached;}
void reflex_swapchain_created(uint64_t sc) {cached.swapchain=sc;cached.latency=cached.frame_marked=true;cached.mode=1;}
''' + reset + r'''
template<class... T> void logf(const char*,T...) {}
void present_thread_drain() {}   // (RX_VK_PRESENT_THREAD off: nothing handed over)
std::atomic<bool> presenter_surface_lost{false};   // (and nothing found lost by it)
int vkCreateSwapchainKHR(int,const CreateInfo* ci,void*,uint64_t* out) {
    if(ci->oldSwapchain) {
        if(!chains.count(ci->oldSwapchain) || retired.count(ci->oldSwapchain))bad_chain=true;
        retired.insert(ci->oldSwapchain); // Vulkan retires oldSwapchain even on creation failure.
    }
    if(create_result)return create_result;
    *out=++next_chain;chains.insert(*out);return 0;
}
int vkGetSwapchainImagesKHR(int,uint64_t,uint32_t* n,uint64_t* out) {
    *n=image_count;if(out)for(unsigned i=0;i<*n;++i)out[i]=i;return out?data_result:query_result;
}
int vkCreateSemaphore(int,const VkSemaphoreCreateInfo*,void*,uint64_t* out) {
    if(++made==fail_at)return -1;*out=++serial;live.insert(*out);return 0;
}
void vkDestroySemaphore(int,uint64_t s,void*) {
    if((used.count(s) && !idle) || !live.erase(s))bad_destroy=true;
}
void vkDestroySwapchainKHR(int,uint64_t sc,void*) {
    if(cached.swapchain==sc)bad_chain=true;
    if(!chains.erase(sc))bad_chain=true;
    retired.erase(sc);destroyed_swapchain=sc;
}
void queue_flush_locked() {}
void vkDeviceWaitIdle(int) {idle=true;}
// (vk_replay.cpp's helper: held submits out, then the device idle; the real one also takes the present queue's own lock)
void device_wait_idle_locked() {queue_flush_locked();vkDeviceWaitIdle(dev.device);}
void swap_views_drop() {}   // (hookshot: the overlays' views, vk_main.cpp)
void vkDestroySurfaceKHR(int, uint64_t surface, void*) {
    if (!surface || !chains.empty() || !live.empty() || !idle) bad_destroy=true;
    ++surfaces_destroyed;
}
''' + retire + '\n' + loss + r'''
bool recreate() {
    uint32_t n=0,width=640,height=480; struct {int format=1;} fmt;
    struct {uint32_t minImageCount=2;} caps;   // (the surface's: present ahead holds images past it, vk_framegen.cpp)
    CreateInfo ci;int mode=0;VkResult result;
''' + tail + r'''
    reflex_swapchain_created(sc);
    return true;
}
uint64_t pack(unsigned w,unsigned h) {return (uint64_t(w)<<32)|h;}
unsigned called_width=0,called_height=0,acquires=0;
bool preflight_fail=false,stale=false;uint64_t newer_resize=0;
bool create_swapchain(unsigned width,unsigned height) {
    called_width=width;called_height=height;
    if(newer_resize)dev.swap_resize.store(newer_resize);
    if(preflight_fail)return false;
    return recreate();
}
bool reflex_swapchain_stale() {return stale;}
int menu_vsync() {return 0;}   // (vk_menu.cpp: the V-Sync row, Auto)
int swapchain_framegen() {return 0;}   // (vk_main.cpp: frame generation's multiplier as the swapchain was made, none)
bool swapchain_dlssg() {return false;}   // (vk_main.cpp: and DLSS-G makes its sets: no)
std::mutex present_image_mu;
struct PendingPresent {
    bool used;
    uint32_t index;
    uint64_t swapchain, swapchain_generation;
    uint64_t reflex_frame = ~0ull;   // (NVIDIA Reflex: the frame it shows; vk_reflex.h reflex_present_frame)
    uint64_t antilag_frame = ~0ull;  // (AMD Anti-Lag 2: the same; vk_antilag.h antilag_present_frame)
};
thread_local uint64_t reflex_present_frame = ~0ull;   // (vk_reflex.cpp: the frame the present shows, kept around present_out)
thread_local uint64_t antilag_present_frame = ~0ull;  // (vk_antilag.cpp: the same for AMD Anti-Lag 2)
PendingPresent* present_defer_into=nullptr;
unsigned committed=0, after_acquire=0;
bool present_out(uint32_t, std::unique_lock<std::mutex>&) {++committed; return true;}
''' + commit + r'''
void acquire_with_result(VkResult res) {
''' + acquire_loss + r'''
    ++after_acquire;
}
bool present_with_result(VkResult res, bool dedicated) {
    std::unique_lock qk(dev.present_mu);
    if (dedicated) qk.unlock();
''' + present_loss + r'''
    return true;
}
void present_begin() {
''' + begin + r'''
    if(!dev.swapchain || !chains.count(dev.swapchain) || retired.count(dev.swapchain))bad_chain=true;
    ++acquires; // The real prefix must return before the frame fence/acquire on failure.
}
struct Frame {uint64_t rendered,acquired;};
uint64_t choose(unsigned index,unsigned frame_slot) {
    Frame f{100+frame_slot,200+frame_slot};struct{uint64_t semaphore;}signal{};
    struct{uint64_t semaphore;}waits[1]{};
    struct{const uint64_t* pWaitSemaphores;}pi{};
''' + signal + '\n' + wait + '\n' + acquire_wait + r'''
    if(signal.semaphore!=*pi.pWaitSemaphores)return 0;
    if(waits[0].semaphore!=200+frame_slot)return 0;
    used.insert(signal.semaphore);
    return signal.semaphore;
}
int main() {
    if(!recreate())return 1;
    uint64_t pending[16]{};unsigned owner[16]{},count=0;
    // Only reacquiring image i releases its previous presentation wait.
    unsigned images[]={0,1,2,3,0,1,2,3,3,1,0,2};
    for(unsigned step=0;step<12;++step) {
        unsigned image=images[step];
        for(unsigned j=0;j<count;++j)if(owner[j]==image)pending[j]=0;
        auto sem=choose(image,step%3);if(!sem)return 2;
        for(unsigned j=0;j<count;++j)if(pending[j]==sem) {
            std::printf("FAIL: slot %u reused semaphore for image %u while image %u still owns it\n",step%3,image,owner[j]);return 3;
        }
        pending[count]=sem;owner[count++]=image;
    }
    if(dev.swap_rendered.size()!=4 || live.size()!=4)return 4;
    if(dev.swap_spare!=2) {std::puts("FAIL: 4 images over a minimum of 2 should leave 2 to hold at once");return 29;}
    auto old=dev.swap_rendered;image_count=2;idle=false;
    if(!recreate() || bad_destroy || dev.swap_rendered.size()!=2 || live.size()!=2)return 5;
    if(dev.swap_spare!=0) {std::puts("FAIL: 2 images over a minimum of 2 leave none to hold at once");return 30;}
    for(auto s:old)if(live.count(s))return 6;
    for(unsigned slot=0;slot<3;++slot)for(unsigned image=0;image<2;++image)
        if(choose(image,slot)!=dev.swap_rendered[image])return 9;
    image_count=5;idle=false;
    if(!recreate() || bad_destroy || dev.swap_rendered.size()!=5 || live.size()!=5)return 10;
    if(dev.swap_spare!=3)return 31;
    for(unsigned image=0;image<5;++image)if(choose(image,image%3)!=dev.swap_rendered[image])return 11;
    // Once creation has been attempted, the old chain cannot be acquired or used as oldSwapchain again.
    image_count=4;made=0;fail_at=3;
    idle=false;
    if(recreate() || dev.swapchain || !dev.swap_rendered.empty() || !live.empty() || !chains.empty()) {
        std::puts("FAIL: retired swapchain remains published after semaphore allocation failure");return 7;
    }
    fail_at=0;made=0;query_result=-1;destroyed_swapchain=0;
    if(recreate() || dev.swapchain || made || !live.empty() || !chains.empty())return 12;
    query_result=0;image_count=0;destroyed_swapchain=0;
    if(recreate() || dev.swapchain || made || !live.empty() || !chains.empty())return 13;
    image_count=4;
    if(!recreate())return 14;
    create_result=-1;idle=false;
    if(recreate() || dev.swapchain || !live.empty() || !chains.empty()) {
        std::puts("FAIL: retired swapchain remains published after creation failure");return 15;
    }
    if(dev.swap_spare) {std::puts("FAIL: images to spare still counted with no swapchain");return 32;}
    create_result=0;
    if(!recreate() || !dev.swapchain || dev.swap_images.size()!=4)return 16;
    data_result=1; // VK_INCOMPLETE from the image-data query must not publish a partial chain.
    if(recreate() || dev.swapchain || !live.empty() || !chains.empty())return 17;
    data_result=0;
    if(!recreate())return 18;
    dev.swap_resize.store(pack(1720,720));preflight_fail=true;
    count=acquires;auto active=dev.swapchain;
    present_begin();
    if(acquires!=count || dev.swap_resize.load()!=pack(1720,720) || dev.swapchain!=active)return 19;
    newer_resize=pack(3440,1440);
    present_begin();
    if(acquires!=count || dev.swap_resize.load()!=newer_resize)return 20;
    preflight_fail=false;newer_resize=0;
    present_begin();
    if(acquires!=count+1 || called_width!=3440 || called_height!=1440 || dev.swap_resize.load())return 21;
    create_result=-1;dev.swap_resize.store(pack(1920,1080));count=acquires;
    present_begin();
    if(acquires!=count || dev.swapchain || dev.swap_resize.load()!=pack(1920,1080))return 22;
    create_result=0;present_begin();
    if(acquires!=count+1 || !dev.swapchain || called_width!=1920)return 23;
    stale=true;create_result=-1;count=acquires;present_begin();
    if(acquires!=count || dev.swapchain || !dev.swap_resize.load())return 24;
    stale=false;create_result=0;dev.swap_resize.store(0);present_begin();
    if(acquires!=count+1 || !dev.swapchain)return 25; // Null-chain recovery without a resize notification.
    opts.hdr=1;preflight_fail=true;count=acquires;active=dev.swapchain;present_begin();
    if(acquires!=count || dev.swapchain!=active || !dev.swap_resize.load())return 27;
    preflight_fail=false;present_begin();
    if(acquires!=count+1 || !dev.swapchain || dev.swap_resize.load())return 28;
    // The display thread chose present-ahead on a chain with two spare images.
    PendingPresent deferred{};
    present_defer_into=&deferred;
    image_count=3;
    dev.swap_resize.store(pack(1280,720));
    count=acquires;
    present_begin();
    if (dev.swap_spare!=1 || acquires!=count) return 40;
    // The next ordinary frame can use the smaller replacement.
    present_defer_into=nullptr;
    present_begin();
    if (acquires!=count+1) return 41;
    const PendingPresent pending_old{true, 0, dev.swapchain, dev.swapchain_generation};
    idle=false;
    acquire_with_result(VK_SUCCESS);
    acquire_with_result(VK_ERROR_SURFACE_LOST_KHR);
    if (after_acquire!=1 || surfaces_destroyed!=1 || dev.surface || dev.swapchain ||
        dev.present_queue_supported || bad_destroy || present_commit(pending_old)) return 33;
    dev.surface=2;
    next_chain=pending_old.swapchain-1;  // A driver is free to reuse the old handle.
    if (!recreate() || dev.swapchain!=pending_old.swapchain || present_commit(pending_old) || committed) return 34;
    const PendingPresent pending_new{true, 0, dev.swapchain, dev.swapchain_generation};
    if (!present_commit(pending_new) || committed!=1) return 35;
    idle=false;
    if (present_with_result(VK_ERROR_SURFACE_LOST_KHR, true) || surfaces_destroyed!=2 ||
        dev.surface || dev.swapchain || bad_destroy || present_commit(pending_new)) return 36;
    if (!present_with_result(VK_SUCCESS, false) || surfaces_destroyed!=2) return 37;
    dev.surface=3;
    if (!recreate()) return 38;
    create_result=VK_ERROR_SURFACE_LOST_KHR;
    idle=false;
    if (recreate() || surfaces_destroyed!=3 || dev.surface || dev.swapchain || bad_destroy) return 39;
    if(bad_destroy || bad_chain)return 8;
    reflex_swapchain_destroyed();
    if(cached.swapchain || cached.latency || cached.frame_marked || cached.mode!=-1)return 26;
    std::puts("PASS: per-image semaphores, retirement, query/allocation/create failures, latest-resize retry and acquire guard");
    std::puts("PASS: acquire/present/create surface loss, ordered destruction, and deferred present invalidation with handle reuse");
    std::puts("PASS: a smaller replacement blocks deferred acquire and allows the ordinary path");
}
'''
with tempfile.TemporaryDirectory(prefix='present-semaphores-') as directory:
    folder = Path(directory)
    cpp, exe = folder/'fixture.cpp', folder/'fixture.exe'
    cpp.write_text(fixture)
    subprocess.run([a.cxx, '-std=c++20', '-include', ENV_ON_H, '-O1', str(cpp), '-o', str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
