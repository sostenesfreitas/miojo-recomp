// (hookshot) AMD FSR 4, one of the temporal upscaler's engines (vk_fsr4.h). The frame chain it draws with is
// vk_temporal.cpp's, the same as NVIDIA DLSS's: the colour as the game's network samples it, the depth, the motion (the
// camera's and the skinned draws' own) and the camera jitter. Here are AMD's library, the upscale context and its dispatch.
// The host side follows the API guide of FidelityFX SDK 2.4 (Kits/FidelityFX/docs/getting-started/ffx-api.md): a Vulkan
// backend description chained to the upscale context description, inputs read in SHADER_READ_ONLY_OPTIMAL, the output
// written in GENERAL. First written for Lichii's OoT branch (madebyjordan, vk_fsr4_upscaler.cpp), GPL-3.0-or-later; its Linux
// path (a Vulkan FSR 4 library through dlopen) by N3oTraX.
//
// AMD's library is looked for in RX_VK_FSR_DIR=<folder> when set, the executable's folder (bin\ beside the launcher), then
// the game folder: amd_fidelityfx_loader_vk.dll, with amd_fidelityfx_upscaler_vk.dll beside it (Linux: a Vulkan FSR 4
// library, amd_fidelityfx_upscaler_vk.so). Of the upscalers it lists for this GPU the newest FSR 4 is taken, never an older
// one in its place. RX_VK_FSR4=0: FSR 4 out for this run.
#include "vk_fsr4.h"

#include <atomic>
#include <cstdlib>

#include "vk_internal.h"
#include "vk_temporal.h"

#ifdef HOOKSHOT_FSR
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "fsr4_tune.h"
#include "spirv_unroll.h"
#include "vk_gprof.h"
#include "vk_paths.h"
#include "vk_texture.h"

#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
#include "upscalers/include/ffx_upscale.h"
#endif
#include "vk_work.h"   // (after the others: the barriers here count as work for full_barrier)

namespace mkwvk {

extern std::atomic<bool> present_native_pq;   // (vk_replay.cpp do_present: the game presents its own HDR10 picture, Native)
VkResult shared_compute_pipeline(const VkComputePipelineCreateInfo* ci, VkPipeline* out);   // (vk_draw.cpp: through the pipeline cache kept across runs)
void pipeline_cache_save_play();   // (vk_draw.cpp)

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
// (what is known of FSR on this device: -1 not yet, else an FsrWhy. Set when AMD's library has been asked)
std::atomic<int> g_why{-1};
}  // namespace

FsrWhy fsr4_why() {
    if (!fsr4_built()) return FSR_NOT_BUILT;
    static const bool env_off = env_switch("RX_VK_FSR4") == 0;
    if (env_off) return FSR_ENV_OFF;
    const int w = g_why.load(std::memory_order_relaxed);
    return w < 0 ? FSR_OK : (FsrWhy)w;
}

#ifdef HOOKSHOT_FSR

namespace {

#ifdef _WIN32
constexpr const char* kLoader = "amd_fidelityfx_loader_vk.dll";
#else
// (linux) no AMD loader exists there: the upscaler library itself, which exports the ffx entry points (a Vulkan FSR 4 build)
constexpr const char* kLoader = "amd_fidelityfx_upscaler_vk.so";
#endif

/// AMD's library, loaded once (the menu's thread may ask first: fsr4_probe), and the upscaler it has for this device
struct Library {
    std::mutex mu;
    bool tried = false, ok = false;
    HMODULE lib = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    uint64_t version_id = 0;
    VkDeviceContext device_context{};
};
// (lib() and st() are never destroyed: Hookshot leaves through ExitProcess / exit() with the device alive, and a destructor
// then would call into AMD's library after its DLL_PROCESS_DETACH, or under a render thread still recording with it)
Library& lib() {
    static Library& l = *new Library;
    return l;
}

/// An upscale context and the descriptions it was made from: it keeps pointers into them until it is destroyed, which
/// happens a few frames after it is let go (temporal_later), so each context has its own
struct Context {
    ffxCreateBackendVKDesc backend{};
    ffxOverrideVersion version{};
#ifndef _WIN32
    ffxCreateBackendVKDesc_DEPRECATED legacy_backend{};   // (linux) the backend description a Vulkan FSR 4 build like fsr4-vk reads
#endif
    ffxCreateContextDescUpscale upscale{};
    ffxContext context = nullptr;
    uint32_t width = 0, height = 0;   // (its output, which is also the most the render size can be)
    uint64_t memory = 0;              // (what AMD's library says it holds)
    // (made for the game's native HDR (present_native_pq): its scene colour goes past 1 there, and the LDR context, given
    // the colour clamped to 1, cut every highlight at the paper white. Then it takes linear light, HDR, with AMD's own
    // exposure, and the chain does not clamp it: hdr())
    bool hdr = false;
    ~Context() {
        if (context) lib().destroy(&context, nullptr);
    }
};

/// The engine's state: used with the temporal upscaler's lock held
struct State {
    std::shared_ptr<Context> context;
    uint32_t in_w = 0, in_h = 0;   // (the inputs' size under this context: the largest render size it has had)
    std::chrono::steady_clock::time_point last{};
    uint64_t evals = 0;
    int told_resets = 0;
};
State& st() {
    static State& s = *new State;   // (never destroyed: lib()'s note)
    return s;
}

void on_message(uint32_t type, const wchar_t* message) {
    char text[512] = {};
    if (message) WideCharToMultiByte(CP_UTF8, 0, message, -1, text, (int)sizeof text - 1, nullptr, nullptr);
    logf("vk: AMD FSR 4: AMD's %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

/// The folders the library is looked for in: RX_VK_FSR_DIR, the executable's (bin\ beside the launcher), the game folder
std::vector<std::string> library_dirs() {
    std::vector<std::string> d;
    if (const char* e = getenv("RX_VK_FSR_DIR"); e && *e) d.emplace_back(e);
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::string s(exe, n);
        d.push_back(s.substr(0, s.find_last_of("\\/")));
    }
    n = game_module_path(exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::string s(exe, n);
        s = s.substr(0, s.find_last_of("\\/"));
        if (d.empty() || d.back() != s) d.push_back(s);
    }
    return d;
}

void* symbol(const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress(lib().lib, name);
#else
    return dlsym(lib().lib, name);
#endif
}

/// AMD's passes tuned as the library makes them (fsr4_tune.h: measured gains, outputs bit for bit AMD's on Adrenalin 26.10.1):
/// the device functions it is handed are ours where it makes and destroys buffers, layouts, shader modules and compute
/// pipelines, writes descriptors and records binds and dispatches. At once: the network's layers 64 lanes wide, and the
/// weights the layers and the postpass copy into per-thread arrays (scratch memory) read from a tail of the library's storage
/// buffer instead. Later: the smaller layers with their loops unrolled from the library's own code (spirv_unroll.h, its
/// weights then constants in the code), built one at a time on a thread of their own and bound in place of the library's once
/// made (about a minute each the first time, the pipeline cache after). On an RX 6700 XT at 720p -> 1440p: 8.97 ms -> 4.04 ms.
/// RX_VK_FSR4_TUNE=0: as AMD made them; RX_VK_FSR4_UNROLL=0: no unrolled layers. On AMD's GPUs only.
/// The tail is kept per buffer: a pipeline reads it only in the buffer it was rewritten against, while that buffer lives and
/// every binding of it seen takes the tail in (offset 0, the whole buffer). Otherwise (the buffer destroyed, a binding that
/// leaves the tail out, or none seen through the functions watched here) AMD's own shader is built and bound in its place,
/// and the log says so once: a slower picture, never a wrong one.
/// The unrolling's limits are what keep it safe: unrolled whole, the larger layers (passes 4, 5, 10) came to 200,000 words
/// and AMD's compiler took 40-55 GB of commit charge for them, three at a time, until a PC with 16 GB of RAM crashed. Now a
/// layer is unrolled only while it stays under kUnrollWords (passes 1, 2 and 12: ~107,000 words, ~3 GB to build), one build
/// runs at a time, only with kBuildHeadroom of memory to spare, and a build that fails is not tried again. The window closing
/// stops them (fsr4_close): one not started does not start, the one building is waited for (10 s at most)
namespace tune {
std::mutex mu;
PFN_vkCreateBuffer real_cb = nullptr;
PFN_vkDestroyBuffer real_db = nullptr;
PFN_vkCreateShaderModule real_csm = nullptr;
PFN_vkDestroyShaderModule real_dsm = nullptr;
PFN_vkCreateComputePipelines real_ccp = nullptr;
PFN_vkCreateDescriptorSetLayout real_cdsl = nullptr;
PFN_vkCreatePipelineLayout real_cpl = nullptr;
PFN_vkDestroyPipeline real_dp = nullptr;
PFN_vkUpdateDescriptorSets real_uds = nullptr;
PFN_vkCmdPushDescriptorSetKHR real_push = nullptr, real_push14 = nullptr;
PFN_vkGetDescriptorEXT real_gd = nullptr;
PFN_vkGetBufferDeviceAddress real_bda = nullptr, real_bda_khr = nullptr;
PFN_vkCmdBindPipeline real_bind = nullptr;
PFN_vkCmdDispatch real_dispatch = nullptr;
PFN_vkCmdUpdateBuffer real_update = nullptr;
PFN_vkCmdPipelineBarrier real_barrier = nullptr;
constexpr uint64_t kTail = 64 << 10;   // (bytes added to the library's big storage buffer: the weights of the layers and the postpass, ~32 KB)

/// The library's big storage buffer (each upscale context makes one) and the tail added to it, by generation: the shaders
/// rewritten against it read the tail at its size, in it alone
struct Big {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t size = 0;                // (as the library asked for it: the tail starts there)
    VkDeviceAddress address = 0;      // (descriptor buffers: where the library was told it is)
    std::vector<uint32_t> tail;       // (what the tail holds)
    bool due = false;                 // (to be written before the first dispatch that reads it)
    int bound = 0;                    // (its bindings seen: 0 none yet, 1 each took the tail in, -1 one left it out)
};
std::unordered_map<uint64_t, Big> bigs;              // (alive ones only: a generation not here was destroyed)
std::unordered_map<VkBuffer, uint64_t> big_of;
uint64_t cur = 0, next_gen = 1;                      // (cur: the newest alive, 0 none: new modules are rewritten against it)
std::unordered_set<uint64_t> told;                   // (generations whose pipelines the log has said fell back)

// (the library's descriptor set layouts and pipeline layouts as made, to make copies from: an unrolled layer's, AMD's own)
struct SetLayoutInfo {
    VkDescriptorSetLayoutCreateFlags flags = 0;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<std::vector<VkSampler>> samplers;
};
std::unordered_map<VkDescriptorSetLayout, SetLayoutInfo> set_layouts;
struct LayoutInfo {
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> push;
};
std::unordered_map<VkPipelineLayout, LayoutInfo> layouts;
/// A layout's definition, whole (the set layouts' too): what a copy is made from, whatever becomes of the library's
struct LayoutCopy {
    std::vector<SetLayoutInfo> sets;
    std::vector<VkPushConstantRange> push;
};

struct Module {
    uint8_t kind = 0;                                       // (flags) 1: 64 lanes, 2: reads the tail
    uint64_t gen = 0;                                       // (2: the Big it reads)
    std::shared_ptr<const std::vector<uint32_t>> code;      // (2: the library's own code, as it gave it)
    std::string entry;
};
std::unordered_map<VkShaderModule, Module> modules;

/// What one of the library's pipelines was made from, its own code: to make it as AMD made it when the tail cannot be read
struct Plain {
    std::shared_ptr<const std::vector<uint32_t>> code;
    std::string entry;
    VkPipelineCreateFlags flags = 0;
    VkPipelineShaderStageCreateFlags stage_flags = 0;
    uint32_t lanes = 0;   // (the lane count it asked for, 0 none)
    bool specialized = false;
    std::vector<VkSpecializationMapEntry> spec_map;
    std::vector<uint8_t> spec_data;
    LayoutCopy layout;
};
struct TailPipe {
    uint64_t gen = 0;
    std::shared_ptr<const Plain> plain;
    VkPipeline twin = VK_NULL_HANDLE;   // (AMD's own, made once the tail could not be read)
    bool twin_failed = false;
};
std::unordered_map<VkPipeline, TailPipe> tail_pipes;
std::atomic<int> tail_count{0};
std::unordered_map<VkCommandBuffer, VkPipeline> bound;   // (the library's pipeline as it bound it)
int layers64 = 0;

// (the unrolled pipelines: made by jobs, bound in the library's place; a job whose pipeline was destroyed meanwhile drops
// what it made)
std::unordered_map<VkPipeline, VkPipeline> better;
std::unordered_set<VkPipeline> pending, cancelled;
std::atomic<int> better_count{0};
/// The jobs: one builds at a time (a first build is a minute of CPU and ~3 GB of the driver's memory). Never destroyed, as
/// lib(): a job still in the driver when the process exits must not find them gone
struct Jobs {
    std::mutex mu;
    std::condition_variable cv;
    int slots = 1;
    int running = 0;   // (threads started and not yet ended: fsr4_close waits for them)
};
Jobs& jobs() {
    static Jobs& j = *new Jobs;
    return j;
}
std::atomic<bool> closing{false};   // (the window closed: no job starts, the one building leaves what it made)
constexpr size_t kUnrollWords = 120000;              // (a layer unrolled to more than this is left rolled)
constexpr uint64_t kBuildHeadroom = 8ull << 30;     // (memory to spare before a build starts)

bool on() {
    static const bool v = env_switch("RX_VK_FSR4_TUNE") != 0 && dev.props.vendorID == 0x1002;
    return v;
}
bool unroll_on() {
    static const bool v = env_switch("RX_VK_FSR4_UNROLL") != 0;
    return v;
}
/// What the system can still commit: Windows' commit charge left (RAM and page file), Linux's available memory and free swap.
/// True when unknown
bool build_headroom(uint64_t* avail) {
    *avail = 0;
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return true;
    *avail = m.ullAvailPageFile;
#else
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return true;
    unsigned long long mem = 0, swap = 0, v = 0;
    char key[64];
    while (fscanf(f, "%63s %llu kB\n", key, &v) == 2) {
        if (!strcmp(key, "MemAvailable:")) mem = v;
        else if (!strcmp(key, "SwapFree:")) swap = v;
    }
    fclose(f);
    *avail = (uint64_t)(mem + swap) << 10;
#endif
    return *avail >= kBuildHeadroom;
}

/// (mu held) A buffer let go, or its handle given out again: if it was a Big, its tail with it. Its pipelines then read none
/// (cmd_dispatch binds AMD's own), and no new module is rewritten against it
void forget_locked(VkBuffer b) {
    auto it = big_of.find(b);
    if (it == big_of.end()) return;
    if (cur == it->second) cur = 0;
    bigs.erase(it->second);
    big_of.erase(it);
}

VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice d, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a, VkBuffer* b) {
    VkBufferCreateInfo c = *ci;
    const bool grow = (ci->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && ci->size > (64ull << 20);
    // (the tail is written with vkCmdUpdateBuffer: its buffer a transfer's destination, whatever the library asked)
    if (grow) c.size += kTail, c.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkResult r = real_cb(d, &c, a, b);
    if (r == VK_SUCCESS) {
        std::lock_guard lk(mu);
        forget_locked(*b);   // (a handle given out before, its destruction not seen: not what it was)
        if (grow) {
            const uint64_t gen = next_gen++;
            Big& g = bigs[gen];
            g.buffer = *b, g.size = ci->size;
            big_of[*b] = gen, cur = gen;
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks* a) {
    if (b) {
        std::lock_guard lk(mu);
        forget_locked(b);
    }
    real_db(d, b, a);
}
/// Where the library is told a Big is (descriptor buffers: its descriptors are made from addresses)
VKAPI_ATTR VkDeviceAddress VKAPI_CALL buffer_address(VkDevice d, const VkBufferDeviceAddressInfo* info) {
    const VkDeviceAddress r = real_bda(d, info);
    std::lock_guard lk(mu);
    if (auto it = big_of.find(info->buffer); it != big_of.end()) bigs[it->second].address = r;
    return r;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL buffer_address_khr(VkDevice d, const VkBufferDeviceAddressInfo* info) {
    const VkDeviceAddress r = real_bda_khr(d, info);
    std::lock_guard lk(mu);
    if (auto it = big_of.find(info->buffer); it != big_of.end()) bigs[it->second].address = r;
    return r;
}

/// (mu held) A storage-buffer binding of a Big seen: whether it takes the tail in (fsr4_tune::covers_tail). A binding of
/// exactly what the library asked for is widened to the tail first (*range), as the buffer is that large
void note_binding_locked(Big& g, uint64_t gen, bool dynamic, uint64_t offset, uint64_t* range) {
    if (!dynamic && *range == g.size && g.size + kTail <= dev.props.limits.maxStorageBufferRange)
        *range = fsr4_tune::widened_range(offset, *range, g.size, kTail);
    const bool ok = !dynamic && fsr4_tune::covers_tail(offset, *range, g.size, kTail);
    if (ok && g.bound == 0) g.bound = 1;
    if (!ok && g.bound != -1) {
        g.bound = -1;
        if (told.insert(gen).second)
            logf("vk: AMD FSR 4: its storage buffer is bound without the tail (%s, offset %llu, range %llu of %llu): AMD's own "
                 "shaders in place of the tuned ones that read it, for this context",
                 dynamic ? "dynamic" : "plain", (unsigned long long)offset, (unsigned long long)*range, (unsigned long long)g.size);
    }
}
/// (mu held) Descriptor writes (vkUpdateDescriptorSets, vkCmdPushDescriptorSet): each binding of a Big noted. When one is
/// widened, the writes to pass on are *out (copies, in `infos`); else the library's own
const VkWriteDescriptorSet* note_writes_locked(uint32_t n, const VkWriteDescriptorSet* w, std::vector<VkWriteDescriptorSet>& out,
                                               std::vector<std::vector<VkDescriptorBufferInfo>>& infos) {
    if (big_of.empty()) return w;
    for (uint32_t i = 0; i < n; i++) {
        const bool dynamic = w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        if ((w[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && !dynamic) || !w[i].pBufferInfo) continue;
        for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
            auto it = big_of.find(w[i].pBufferInfo[k].buffer);
            if (it == big_of.end()) continue;
            uint64_t range = w[i].pBufferInfo[k].range;
            note_binding_locked(bigs[it->second], it->second, dynamic, w[i].pBufferInfo[k].offset, &range);
            if (range == w[i].pBufferInfo[k].range) continue;
            if (out.empty()) out.assign(w, w + n), infos.resize(n);
            if (infos[i].empty()) infos[i].assign(w[i].pBufferInfo, w[i].pBufferInfo + w[i].descriptorCount), out[i].pBufferInfo = infos[i].data();
            infos[i][k].range = range;
        }
    }
    return out.empty() ? w : out.data();
}
VKAPI_ATTR void VKAPI_CALL update_sets(VkDevice d, uint32_t n, const VkWriteDescriptorSet* w, uint32_t copies, const VkCopyDescriptorSet* c) {
    std::vector<VkWriteDescriptorSet> out;
    std::vector<std::vector<VkDescriptorBufferInfo>> infos;
    {
        std::lock_guard lk(mu);
        w = note_writes_locked(n, w, out, infos);
    }
    real_uds(d, n, w, copies, c);
}
VKAPI_ATTR void VKAPI_CALL push_set(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n,
                                    const VkWriteDescriptorSet* w) {
    std::vector<VkWriteDescriptorSet> out;
    std::vector<std::vector<VkDescriptorBufferInfo>> infos;
    {
        std::lock_guard lk(mu);
        w = note_writes_locked(n, w, out, infos);
    }
    real_push(cb, bp, l, set, n, w);
}
VKAPI_ATTR void VKAPI_CALL push_set14(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n,
                                      const VkWriteDescriptorSet* w) {
    std::vector<VkWriteDescriptorSet> out;
    std::vector<std::vector<VkDescriptorBufferInfo>> infos;
    {
        std::lock_guard lk(mu);
        w = note_writes_locked(n, w, out, infos);
    }
    real_push14(cb, bp, l, set, n, w);
}
/// (descriptor buffers) A storage buffer's descriptor made from an address in a Big: noted as a binding of it
VKAPI_ATTR void VKAPI_CALL get_descriptor(VkDevice d, const VkDescriptorGetInfoEXT* info, size_t size, void* out) {
    VkDescriptorGetInfoEXT copy;
    VkDescriptorAddressInfoEXT address;
    if (info && info->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && info->data.pStorageBuffer) {
        std::lock_guard lk(mu);
        const VkDescriptorAddressInfoEXT& at = *info->data.pStorageBuffer;
        for (auto& [gen, g] : bigs) {
            if (!g.address || at.address < g.address || at.address >= g.address + g.size + kTail) continue;
            uint64_t range = at.range;
            note_binding_locked(g, gen, false, at.address - g.address, &range);
            if (range != at.range) {
                copy = *info, address = at, address.range = range, copy.data.pStorageBuffer = &address;
                info = &copy;
            }
            break;
        }
    }
    real_gd(d, info, size, out);
}

VKAPI_ATTR VkResult VKAPI_CALL create_set_layout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* ci, const VkAllocationCallbacks* a,
                                                 VkDescriptorSetLayout* l) {
    const VkResult r = real_cdsl(d, ci, a, l);
    if (r == VK_SUCCESS) {
        std::lock_guard lk(mu);
        set_layouts.erase(*l);   // (a handle the driver gave out before: not what it was)
    }
    if (r == VK_SUCCESS && !ci->pNext) {   // (one with extension structures is not copied: its pipelines are left as they are)
        SetLayoutInfo s;
        s.flags = ci->flags;
        s.bindings.assign(ci->pBindings, ci->pBindings + ci->bindingCount);
        s.samplers.resize(ci->bindingCount);
        for (uint32_t i = 0; i < ci->bindingCount; i++)
            if (ci->pBindings[i].pImmutableSamplers)
                s.samplers[i].assign(ci->pBindings[i].pImmutableSamplers, ci->pBindings[i].pImmutableSamplers + ci->pBindings[i].descriptorCount);
        std::lock_guard lk(mu);
        set_layouts[*l] = std::move(s);
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL create_pipeline_layout(VkDevice d, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks* a,
                                                      VkPipelineLayout* l) {
    const VkResult r = real_cpl(d, ci, a, l);
    if (r == VK_SUCCESS) {
        std::lock_guard lk(mu);
        layouts.erase(*l);
    }
    if (r == VK_SUCCESS && !ci->pNext) {
        LayoutInfo li;
        li.sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
        li.push.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
        std::lock_guard lk(mu);
        layouts[*l] = std::move(li);
    }
    return r;
}
/// (mu held) A pipeline layout of the library's, whole: false when it or one of its set layouts was not copied
bool layout_copy_locked(VkPipelineLayout l, LayoutCopy* out) {
    auto lay = layouts.find(l);
    if (lay == layouts.end()) return false;
    out->sets.clear();
    for (VkDescriptorSetLayout s : lay->second.sets) {
        auto sl = set_layouts.find(s);
        if (sl == set_layouts.end()) return false;
        out->sets.push_back(sl->second);
    }
    out->push = lay->second.push;
    return true;
}
/// A layout of our own, defined as the library's was: compatible with the sets it binds
bool make_layout(VkDevice d, const LayoutCopy& lc, VkPipelineLayout* layout, std::vector<VkDescriptorSetLayout>* sets) {
    *layout = VK_NULL_HANDLE;
    bool ok = true;
    for (const SetLayoutInfo& s : lc.sets) {
        std::vector<VkDescriptorSetLayoutBinding> b = s.bindings;
        for (size_t k = 0; k < b.size(); k++) b[k].pImmutableSamplers = s.samplers[k].empty() ? nullptr : s.samplers[k].data();
        VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sci.flags = s.flags, sci.bindingCount = (uint32_t)b.size(), sci.pBindings = b.data();
        VkDescriptorSetLayout copy = VK_NULL_HANDLE;
        if (real_cdsl(d, &sci, nullptr, &copy) != VK_SUCCESS) { ok = false; break; }
        sets->push_back(copy);
    }
    if (ok) {
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = (uint32_t)sets->size(), lci.pSetLayouts = sets->data();
        lci.pushConstantRangeCount = (uint32_t)lc.push.size(), lci.pPushConstantRanges = lc.push.data();
        ok = real_cpl(d, &lci, nullptr, layout) == VK_SUCCESS;
    }
    if (!ok) {
        for (VkDescriptorSetLayout s : *sets) vkDestroyDescriptorSetLayout(d, s, nullptr);
        sets->clear();
    }
    return ok;
}

VKAPI_ATTR VkResult VKAPI_CALL create_shader_module(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a,
                                                   VkShaderModule* m) {
    const uint32_t* code = ci->pCode;
    const size_t words = ci->codeSize / 4;
    const std::string entry = fsr4_tune::entry_name(code, words);
    Module mod;
    mod.entry = entry;
    std::vector<uint32_t> tuned;
    VkShaderModuleCreateInfo c = *ci;
    {
        std::lock_guard lk(mu);
        if (fsr4_tune::wants_wave64(entry) && dev.subgroup_max >= 64 && !fsr4_tune::lane_dependent(code, words)) mod.kind |= 1;
        // (the layers and the postpass copy constant weight tables into per-thread arrays, which the driver keeps in scratch
        // memory: read from the tail of the newest Big instead, while it has room)
        std::vector<uint32_t> add;
        auto g = bigs.find(cur);
        if ((fsr4_tune::is_postpass(entry) || fsr4_tune::wants_wave64(entry)) && g != bigs.end() &&
            fsr4_tune::arrays_to_buffer(code, words, (uint32_t)(g->second.size / 4 + g->second.tail.size()), tuned, add)) {
            std::vector<uint32_t>& tail = g->second.tail;
            if ((tail.size() + add.size()) * 4 <= kTail) {
                tail.insert(tail.end(), add.begin(), add.end());
                c.codeSize = tuned.size() * 4, c.pCode = tuned.data(), mod.kind |= 2, mod.gen = cur, g->second.due = true;
                logf("vk: AMD FSR 4: tuned %s: its constant arrays read from the end of its storage buffer (%zu words there)",
                     entry.c_str(), tail.size());
            } else {
                logf("vk: AMD FSR 4: %s: its constant arrays left as they were (the tail is full)", entry.c_str());
            }
        }
    }
    // (its own code kept: AMD's pipeline made from it when the tail cannot be read, and the layers unrolled from it)
    if (mod.kind & 2) mod.code = std::make_shared<const std::vector<uint32_t>>(code, code + words);
    const VkResult r = real_csm(d, &c, a, m);
    if (r == VK_SUCCESS) {
        std::lock_guard lk(mu);
        modules.erase(*m);   // (a handle given out before)
        if (mod.kind) modules[*m] = std::move(mod);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroy_shader_module(VkDevice d, VkShaderModule m, const VkAllocationCallbacks* a) {
    if (m) {
        std::lock_guard lk(mu);
        modules.erase(m);
    }
    real_dsm(d, m, a);
}

/// AMD's own pipeline, from the library's code and a copy of its layout: bound in place of one whose tail cannot be read.
/// VK_NULL_HANDLE when it could not be made
VkPipeline make_plain(const Plain& p) {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> sets;
    if (!make_layout(dev.device, p.layout, &layout, &sets)) return VK_NULL_HANDLE;
    VkPipeline made = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = p.code->size() * 4, mci.pCode = p.code->data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (real_csm(dev.device, &mci, nullptr, &module) == VK_SUCCESS) {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo lanes{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        lanes.requiredSubgroupSize = p.lanes;
        VkSpecializationInfo spec{};
        spec.mapEntryCount = (uint32_t)p.spec_map.size(), spec.pMapEntries = p.spec_map.data();
        spec.dataSize = p.spec_data.size(), spec.pData = p.spec_data.data();
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.flags = p.flags & ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.pNext = p.lanes ? &lanes : nullptr;
        ci.stage.flags = p.stage_flags, ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = module;
        ci.stage.pName = p.entry.c_str(), ci.stage.pSpecializationInfo = p.specialized ? &spec : nullptr;
        ci.layout = layout, ci.basePipelineIndex = -1;
        if (real_ccp(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &made) != VK_SUCCESS) made = VK_NULL_HANDLE;
        real_dsm(dev.device, module, nullptr);
    }
    vkDestroyPipelineLayout(dev.device, layout, nullptr);
    for (VkDescriptorSetLayout s : sets) vkDestroyDescriptorSetLayout(dev.device, s, nullptr);
    return made;
}

/// A job: the layer unrolled and built with copies of its layouts; bound in the library's pipeline's place when made. Its
/// thread counted in jobs().running from before it starts (fsr4_close waits for it)
void build_unrolled(VkPipeline original, std::shared_ptr<const std::vector<uint32_t>> code, std::string name, VkComputePipelineCreateInfo ci,
                    VkPipelineLayout layout, std::vector<VkDescriptorSetLayout> sets) {
    struct Ended {
        ~Ended() {
            Jobs& j = jobs();
            { std::lock_guard lk(j.mu); j.running--; }
            j.cv.notify_all();
        }
    } ended;
    Jobs& j = jobs();
    {
        std::unique_lock lk(j.mu);
        j.cv.wait(lk, [&j] { return j.slots > 0 || closing.load(); });
        if (closing) return;   // (the window closed: not started. What it holds goes with the process)
        j.slots--;
    }
    struct Slot {
        ~Slot() {
            Jobs& j = jobs();
            { std::lock_guard lk(j.mu); j.slots++; }
            j.cv.notify_all();
        }
    } slot;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t> unrolled;
    int left = -1;
    const bool ok = spirv_unroll::unroll_all(code->data(), code->size(), unrolled, kUnrollWords, 64, &left) && left == 0;
    VkPipeline made = VK_NULL_HANDLE;
    uint64_t avail = 0;
    const bool room = ok && !closing && build_headroom(&avail);
    if (ok && !room && !closing)
        logf("vk: AMD FSR 4: %s: not unrolled now: %llu MB of memory to spare, %llu wanted (the next start tries again)",
             name.c_str(), (unsigned long long)(avail >> 20), (unsigned long long)(kBuildHeadroom >> 20));
    if (room) {
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = unrolled.size() * 4, mci.pCode = unrolled.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (real_csm(dev.device, &mci, nullptr, &module) == VK_SUCCESS) {
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo lanes{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
            lanes.requiredSubgroupSize = 64;
            ci.stage.pNext = &lanes, ci.stage.module = module, ci.stage.pName = name.c_str(), ci.layout = layout;
            ci.stage.pSpecializationInfo = nullptr;   // (only layers without one are taken)
            ci.basePipelineHandle = VK_NULL_HANDLE, ci.basePipelineIndex = -1;
            ci.flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
            // (through the pipeline cache kept across runs, as FSR 3.1's: built once, loaded after. A failure is not tried again:
            // the library's pipeline stays)
            const VkResult rr = shared_compute_pipeline(&ci, &made);
            // (the window closed while the driver built it: fsr4_close may have stopped waiting, and the process be ending.
            // Nothing more is touched; the pipeline cache's save at the close has what the driver made)
            if (closing) return;
            if (rr != VK_SUCCESS) made = VK_NULL_HANDLE, logf("vk: AMD FSR 4: %s: its unrolled pipeline was not built (%d): AMD's stays", name.c_str(), (int)rr);
            real_dsm(dev.device, module, nullptr);
        }
    }
    if (closing) return;
    vkDestroyPipelineLayout(dev.device, layout, nullptr);
    for (VkDescriptorSetLayout s : sets) vkDestroyDescriptorSetLayout(dev.device, s, nullptr);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard lk(mu);
        pending.erase(original);
        if (cancelled.erase(original) || !made) {
            if (made) real_dp(dev.device, made, nullptr);
        } else {
            better[original] = made;
            better_count++;
        }
    }
    // (each one kept as soon as it is built, whether or not the game closes normally: a build of a minute is not done again)
    if (made && s > 1.0) pipeline_cache_save_play();
    if (made) logf("vk: AMD FSR 4: tuned %s: its loops unrolled (%zu -> %zu words), bound in its place from now (%.1f s to build)",
                   name.c_str(), code->size(), unrolled.size(), s);
    else if (!ok) logf("vk: AMD FSR 4: %s left rolled (%d loops kept: unrolled whole it would pass %zu words, or they cannot be)",
                       name.c_str(), left, kUnrollWords);
}

VKAPI_ATTR VkResult VKAPI_CALL create_compute_pipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                       const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks* a,
                                                       VkPipeline* p) {
    std::vector<VkComputePipelineCreateInfo> v(ci, ci + n);
    std::vector<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo> lanes(n);
    std::vector<uint8_t> kind(n, 0);
    std::vector<uint64_t> gen(n, 0);
    std::vector<std::shared_ptr<const std::vector<uint32_t>>> code(n);
    std::vector<std::string> entry(n);
    std::vector<std::shared_ptr<Plain>> plain(n);
    std::vector<VkShaderModule> own(n, VK_NULL_HANDLE);   // (modules of the library's own code, made here: destroyed after)
    {
        std::lock_guard lk(mu);
        for (uint32_t i = 0; i < n; i++) {
            auto k = modules.find(ci[i].stage.module);
            if (k == modules.end()) continue;
            kind[i] = k->second.kind, gen[i] = k->second.gen, code[i] = k->second.code, entry[i] = k->second.entry;
            const VkBaseInStructure* s = (const VkBaseInStructure*)ci[i].stage.pNext;
            if (kind[i] & 2) {
                // (one that reads the tail: what AMD's own is made from, kept. A creation that carries more than a lane count
                // cannot be made again from that: AMD's own code then, now)
                auto pl = std::make_shared<Plain>();
                const bool simple = !ci[i].pNext && (!s || (s->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO && !s->pNext));
                if (simple && layout_copy_locked(ci[i].layout, &pl->layout)) {
                    pl->code = code[i], pl->entry = ci[i].stage.pName ? ci[i].stage.pName : "main";
                    pl->flags = ci[i].flags, pl->stage_flags = ci[i].stage.flags;
                    pl->lanes = s ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*)s)->requiredSubgroupSize : 0;
                    if (const VkSpecializationInfo* sp = ci[i].stage.pSpecializationInfo) {
                        pl->specialized = true;
                        pl->spec_map.assign(sp->pMapEntries, sp->pMapEntries + sp->mapEntryCount);
                        pl->spec_data.assign((const uint8_t*)sp->pData, (const uint8_t*)sp->pData + sp->dataSize);
                    }
                    plain[i] = std::move(pl);
                } else {
                    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    mci.codeSize = code[i]->size() * 4, mci.pCode = code[i]->data();
                    if (real_csm(d, &mci, nullptr, &own[i]) != VK_SUCCESS) own[i] = VK_NULL_HANDLE;
                    if (own[i]) v[i].stage.module = own[i], kind[i] &= ~2;
                }
            }
            if (!(kind[i] & 1)) continue;
            // (its own lane count in place of the one asked: first in the stage's chain, or put first)
            if (s && s->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO) {
                bool deeper = false;
                for (const VkBaseInStructure* t = s; t; t = t->pNext)
                    deeper |= t->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
                if (deeper) { kind[i] &= ~1; continue; }   // (not where it can be swapped: left as asked)
            }
            lanes[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
            lanes[i].requiredSubgroupSize = 64;
            lanes[i].pNext = (void*)(s && s->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO ? s->pNext : s);
            v[i].stage.pNext = &lanes[i];
            v[i].stage.flags &= ~(VkPipelineShaderStageCreateFlags)VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT;
            layers64++;
        }
    }
    const VkResult r = real_ccp(d, cache, n, v.data(), a, p);
    for (VkShaderModule m : own)
        if (m) real_dsm(d, m, nullptr);
    if (r != VK_SUCCESS) return r;
    std::lock_guard lk(mu);
    for (uint32_t i = 0; i < n; i++) {
        if (kind[i] & 2) {   // (without a Plain, only when the library's own module could not be made: no fallback then)
            auto [t, added] = tail_pipes.try_emplace(p[i]);
            if (added) tail_count++;
            t->second = TailPipe{gen[i], std::move(plain[i])};
            t->second.twin_failed = !t->second.plain;
        }
        // (a layer with weight tables, from the library's own code: unrolled, its weights become constants in the code, which
        // is where the gain is. One without tables gains nothing: pass 3, 0.07 ms either way. Only one whose creation carried
        // nothing else: no specialization, no other structures)
        if (!(kind[i] & 1) || !(kind[i] & 2) || !unroll_on() || closing || ci[i].stage.pSpecializationInfo || lanes[i].pNext || ci[i].pNext) continue;
        // (copies of its layouts, made now while the library's are alive: identically defined, so compatible with its sets)
        LayoutCopy lc;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> sets;
        if (!layout_copy_locked(ci[i].layout, &lc) || !make_layout(d, lc, &layout, &sets)) continue;
        pending.insert(p[i]);
        VkComputePipelineCreateInfo job = ci[i];
        job.pNext = nullptr, job.stage.pNext = nullptr;
        { std::lock_guard jl(jobs().mu); jobs().running++; }
        std::thread(build_unrolled, p[i], code[i], entry[i], job, layout, std::move(sets)).detach();   // (counted: fsr4_close)
    }
    static bool told_lanes = false;
    if (!told_lanes && layers64) told_lanes = true, logf("vk: AMD FSR 4: tuned: its network's layers run 64 lanes wide (%d so far)", layers64);
    return r;
}
VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice d, VkPipeline pl, const VkAllocationCallbacks* a) {
    if (pl) {
        std::lock_guard lk(mu);
        auto b = better.find(pl);
        if (b != better.end()) real_dp(d, b->second, nullptr), better.erase(b), better_count--;
        if (pending.count(pl)) cancelled.insert(pl);
        auto t = tail_pipes.find(pl);
        if (t != tail_pipes.end()) {
            if (t->second.twin) real_dp(d, t->second.twin, nullptr);
            tail_pipes.erase(t), tail_count--;
        }
    }
    real_dp(d, pl, a);
}
VKAPI_ATTR void VKAPI_CALL cmd_bind_pipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE && (tail_count.load(std::memory_order_relaxed) || better_count.load(std::memory_order_relaxed))) {
        std::lock_guard lk(mu);
        if (bound.size() > 256) bound.clear();   // (command buffers of the renderer's, made and freed: only the last bind matters)
        bound[cb] = pl;
        auto b = better.find(pl);
        if (b != better.end()) pl = b->second;
    }
    real_bind(cb, bp, pl);
}
/// Before a dispatch of a pipeline that reads the tail: the tail written into its Big if due, while that Big lives and its
/// bindings take the tail in; else AMD's own pipeline bound in its place (made the first time). An unrolled one bound in
/// its place reads no tail
VKAPI_ATTR void VKAPI_CALL cmd_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (tail_count.load(std::memory_order_relaxed)) {
        VkPipeline use = VK_NULL_HANDLE, original = VK_NULL_HANDLE;
        std::shared_ptr<const Plain> make;
        {
            std::lock_guard lk(mu);
            auto b = bound.find(cb);
            auto t = b == bound.end() ? tail_pipes.end() : tail_pipes.find(b->second);
            if (t != tail_pipes.end() && !better.count(b->second)) {
                auto g = bigs.find(t->second.gen);
                if (g != bigs.end() && g->second.bound == 1) {
                    Big& big = g->second;
                    if (big.due) {   // (the tail, before its first reader)
                        const size_t bytes = big.tail.size() * 4;
                        for (size_t o = 0; o < bytes; o += 65536)
                            real_update(cb, big.buffer, big.size + o, std::min<size_t>(65536, bytes - o), (const char*)big.tail.data() + o);
                        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        real_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                        big.due = false;
                    }
                } else {
                    if (told.insert(t->second.gen).second)
                        logf("vk: AMD FSR 4: %s: AMD's own shaders in place of the tuned ones that read it, for this context",
                             g == bigs.end() ? "the storage buffer they were tuned for is gone"
                                             : g->second.bound == 0 ? "its storage buffer was not seen bound (a way of binding not checked here)"
                                                                    : "its storage buffer is bound without the tail");
                    if (t->second.twin) use = t->second.twin;
                    else if (!t->second.twin_failed) make = t->second.plain, original = b->second;
                }
            }
        }
        if (make) {   // (made outside the lock: a compile)
            const VkPipeline made = make_plain(*make);
            std::lock_guard lk(mu);
            auto t = tail_pipes.find(original);
            if (t == tail_pipes.end() || t->second.twin) {
                if (made) real_dp(dev.device, made, nullptr);
                if (t != tail_pipes.end()) use = t->second.twin;
            } else if (made) {
                t->second.twin = use = made;
            } else {
                t->second.twin_failed = true;
                logf("vk: AMD FSR 4: AMD's own pipeline could not be made again: the tuned one stays");
            }
        }
        if (use) real_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, use);
    }
    real_dispatch(cb, x, y, z);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char* name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (!f || !name) return f;
    if (!strcmp(name, "vkCreateBuffer")) return real_cb = (PFN_vkCreateBuffer)f, (PFN_vkVoidFunction)&create_buffer;
    if (!strcmp(name, "vkDestroyBuffer")) return real_db = (PFN_vkDestroyBuffer)f, (PFN_vkVoidFunction)&destroy_buffer;
    if (!strcmp(name, "vkGetBufferDeviceAddress")) return real_bda = (PFN_vkGetBufferDeviceAddress)f, (PFN_vkVoidFunction)&buffer_address;
    if (!strcmp(name, "vkGetBufferDeviceAddressKHR"))
        return real_bda_khr = (PFN_vkGetBufferDeviceAddress)f, (PFN_vkVoidFunction)&buffer_address_khr;
    if (!strcmp(name, "vkUpdateDescriptorSets")) return real_uds = (PFN_vkUpdateDescriptorSets)f, (PFN_vkVoidFunction)&update_sets;
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR")) return real_push = (PFN_vkCmdPushDescriptorSetKHR)f, (PFN_vkVoidFunction)&push_set;
    if (!strcmp(name, "vkCmdPushDescriptorSet")) return real_push14 = (PFN_vkCmdPushDescriptorSetKHR)f, (PFN_vkVoidFunction)&push_set14;
    if (!strcmp(name, "vkGetDescriptorEXT")) return real_gd = (PFN_vkGetDescriptorEXT)f, (PFN_vkVoidFunction)&get_descriptor;
    if (!strcmp(name, "vkCreateShaderModule")) return real_csm = (PFN_vkCreateShaderModule)f, (PFN_vkVoidFunction)&create_shader_module;
    if (!strcmp(name, "vkDestroyShaderModule")) return real_dsm = (PFN_vkDestroyShaderModule)f, (PFN_vkVoidFunction)&destroy_shader_module;
    if (!strcmp(name, "vkCreateComputePipelines"))
        return real_ccp = (PFN_vkCreateComputePipelines)f, (PFN_vkVoidFunction)&create_compute_pipelines;
    if (!strcmp(name, "vkCreateDescriptorSetLayout"))
        return real_cdsl = (PFN_vkCreateDescriptorSetLayout)f, (PFN_vkVoidFunction)&create_set_layout;
    if (!strcmp(name, "vkCreatePipelineLayout")) return real_cpl = (PFN_vkCreatePipelineLayout)f, (PFN_vkVoidFunction)&create_pipeline_layout;
    if (!strcmp(name, "vkDestroyPipeline")) return real_dp = (PFN_vkDestroyPipeline)f, (PFN_vkVoidFunction)&destroy_pipeline;
    if (!strcmp(name, "vkCmdBindPipeline")) return real_bind = (PFN_vkCmdBindPipeline)f, (PFN_vkVoidFunction)&cmd_bind_pipeline;
    if (!strcmp(name, "vkCmdDispatch")) return real_dispatch = (PFN_vkCmdDispatch)f, (PFN_vkVoidFunction)&cmd_dispatch;
    return f;
}
void prepare_device() {   // (what the hooks call that the library may never ask for)
    real_update = (PFN_vkCmdUpdateBuffer)vkGetDeviceProcAddr(dev.device, "vkCmdUpdateBuffer");
    real_barrier = (PFN_vkCmdPipelineBarrier)vkGetDeviceProcAddr(dev.device, "vkCmdPipelineBarrier");
    real_dp = (PFN_vkDestroyPipeline)vkGetDeviceProcAddr(dev.device, "vkDestroyPipeline");
    real_csm = (PFN_vkCreateShaderModule)vkGetDeviceProcAddr(dev.device, "vkCreateShaderModule");
    real_dsm = (PFN_vkDestroyShaderModule)vkGetDeviceProcAddr(dev.device, "vkDestroyShaderModule");
    real_ccp = (PFN_vkCreateComputePipelines)vkGetDeviceProcAddr(dev.device, "vkCreateComputePipelines");
    real_cdsl = (PFN_vkCreateDescriptorSetLayout)vkGetDeviceProcAddr(dev.device, "vkCreateDescriptorSetLayout");
    real_cpl = (PFN_vkCreatePipelineLayout)vkGetDeviceProcAddr(dev.device, "vkCreatePipelineLayout");
}
/// The window closing (fsr4_close): no job starts from now, and the one building is waited for, 10 s at most (as the
/// shaders' preparation is, vk_main.cpp): a driver can take longer on one pipeline, and that job then touches nothing more
void close() {
    if (closing.exchange(true)) return;
    Jobs& j = jobs();
    std::unique_lock lk(j.mu);
    j.cv.notify_all();
    if (!j.running) return;
    logf("vk: AMD FSR 4: the window was closed while %d of its layers were waiting or being unrolled: stopping them", j.running);
    if (!j.cv.wait_for(lk, std::chrono::seconds(10), [&j] { return j.running == 0; }))
        logf("vk: AMD FSR 4: an unrolled layer still building after 10 s: closing anyway (it is left to the driver)");
}
}  // namespace tune

/// AMD's library on this device, once: false when FSR 4 cannot run here (g_why says why, the log too)
bool load() {
    Library& l = lib();
    std::lock_guard lk(l.mu);
    if (l.tried) return l.ok;
    l.tried = true;
    const auto no = [](FsrWhy why) {
        g_why = why;
        return false;
    };
    std::string where;
    for (const std::string& dir : library_dirs()) {
        where += (where.empty() ? "" : ", ") + dir;
#ifdef _WIN32
        const std::string path = dir + "\\" + kLoader;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        // (the upscaler DLL the loader picks lies beside it: the loader's folder first in the search)
        l.lib = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        // (there but not loaded: a DLL it needs missing, a bad signature, or a relative RX_VK_FSR_DIR, which the search
        // flags refuse)
        if (!l.lib) logf("vk: AMD FSR 4: %s not loaded (Windows error %lu)", path.c_str(), (unsigned long)GetLastError());
#else
        const std::string path = dir + "/" + kLoader;
        if (access(path.c_str(), R_OK) != 0) continue;
        l.lib = (HMODULE)dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!l.lib) logf("vk: AMD FSR 4: %s not loaded: %s", path.c_str(), dlerror());
#endif
        if (l.lib) {
            logf("vk: AMD FSR 4: %s", path.c_str());
            break;
        }
    }
    if (!l.lib) {
        logf("vk: AMD FSR 4: no %s in %s (the readme says where AMD's files come from)", kLoader, where.c_str());
        return no(FSR_NO_LIBRARY);
    }
    l.create = (PfnFfxCreateContext)symbol("ffxCreateContext");
    l.destroy = (PfnFfxDestroyContext)symbol("ffxDestroyContext");
    l.query = (PfnFfxQuery)symbol("ffxQuery");
    l.dispatch = (PfnFfxDispatch)symbol("ffxDispatch");
    if (!l.create || !l.destroy || !l.query || !l.dispatch) {
        logf("vk: AMD FSR 4: %s lacks the FSR API's entry points", kLoader);
        return no(FSR_NO_LIBRARY);
    }
    l.device_context = VkDeviceContext{dev.device, dev.physical, vkGetDeviceProcAddr};
    if (tune::on()) {
        tune::prepare_device();
        l.device_context.vkDeviceProcAddr = &tune::device_proc;
    }
    // (the library lists every upscaler this device can run: the newest 4.x is taken explicitly, so an FSR 3.1 provider never
    // stands in silently)
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &l.device_context;
    uint64_t count = 0;
    versions.outputCount = &count;
    if (l.query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count) {
        logf("vk: AMD FSR 4: AMD's library lists no upscaler for this GPU");
        return no(FSR_NO_MODEL);
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (l.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        logf("vk: AMD FSR 4: the upscalers could not be listed");
        return no(FSR_NO_MODEL);
    }
    std::string listed;
    unsigned best[3] = {0, 0, 0};
    for (uint64_t i = 0; i < count; i++) {
        const char* name = names[i] ? names[i] : "";
        listed += (listed.empty() ? "" : ", ") + std::string(name);
        unsigned v[3] = {0, 0, 0};
        sscanf(name, "%u.%u.%u", &v[0], &v[1], &v[2]);
        // (the first of equal versions: a Vulkan FSR 4 build lists its Auto, which picks the GPU's model, before INT8 and FP8)
        if (v[0] == 4 && (!l.version_id || v[1] > best[1] || (v[1] == best[1] && v[2] > best[2]))) {
            std::memcpy(best, v, sizeof best);
            l.version_id = ids[i];
        }
    }
    if (!l.version_id) {
        logf("vk: AMD FSR 4: no FSR 4 for this GPU (AMD's library has: %s)", listed.c_str());
        return no(FSR_NO_MODEL);
    }
    logf("vk: AMD FSR 4: upscalers %s; using %u.%u.%u", listed.c_str(), best[0], best[1], best[2]);
    l.ok = true;
    g_why = FSR_OK;
    return true;
}

/// A context for outputs of w x h, any render size up to that (the game's dynamic resolution and the Render resolution row
/// change it every so often: one context takes them all)
std::shared_ptr<Context> make_context(uint32_t w, uint32_t h, bool hdr) {
    Library& l = lib();
    auto c = std::make_shared<Context>();
    c->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    c->backend.deviceContext = l.device_context;
    c->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    c->version.versionId = l.version_id;
    c->backend.header.pNext = &c->version.header;
#ifndef _WIN32
    // (linux) fsr4-vk looks for the SDK's older backend description, not ffxCreateBackendVKDesc: without it, it finds no device
    // and creation returns FFX_API_RETURN_ERROR_PARAMETER. Both stay chained
    c->legacy_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    c->legacy_backend.vkDevice = l.device_context.vkDevice;
    c->legacy_backend.vkPhysicalDevice = l.device_context.vkPhysicalDevice;
    c->legacy_backend.vkDeviceProcAddr = l.device_context.vkDeviceProcAddr;
    c->version.header.pNext = &c->legacy_backend.header;
#endif
    c->upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    c->upscale.header.pNext = &c->backend.header;
    // (the chain's colour is tonemapped and square-root encoded already: LDR, perceptual, no auto exposure; depth not
    // inverted. The render size changes under the one context. With the game's native HDR the chain gives linear light
    // past 1 (TemporalEngine::hdr): an HDR context, linear, with AMD's auto exposure)
    c->upscale.flags = (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                            : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
                       FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    c->upscale.maxRenderSize = {w, h};
    c->upscale.maxUpscaleSize = {w, h};
    c->upscale.fpMessage = &on_message;
    const auto t0 = std::chrono::steady_clock::now();
    const ffxReturnCode_t r = l.create(&c->context, &c->upscale.header, nullptr);
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != FFX_API_RETURN_OK || !c->context) {
        logf("vk: AMD FSR 4: its upscaler was not made (%d) for %ux%u", (int)r, w, h);
        c->context = nullptr;
        return nullptr;
    }
    c->width = w, c->height = h, c->hdr = hdr;
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescUpscaleGetGPUMemoryUsage mem{};
    mem.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mem.gpuMemoryUsageUpscaler = &usage;
    if (l.query(&c->context, &mem.header) == FFX_API_RETURN_OK) c->memory = usage.totalUsageInBytes;
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool known = l.query(&c->context, &provider.header) == FFX_API_RETURN_OK && provider.versionName;
    logf("vk: AMD FSR 4: FSR %s made for outputs of %ux%u (frame %llu, %.1f ms of CPU; its own %llu MB)%s",
         known ? provider.versionName : "?", w, h, (unsigned long long)texture_frame_no(), made_ms,
         (unsigned long long)(c->memory >> 20), hdr ? ", HDR colour, linear (the game's native HDR)" : "");
    return c;
}

FfxApiResource resource(const TemporalImage& im, VkImageUsageFlags usage, uint32_t state) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = im.format, ci.extent = {im.width, im.height, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage, ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ffxApiGetResourceVK((void*)im.image, ffxApiGetImageResourceDescriptionVK(im.image, ci, 0), state);
}

VkImageMemoryBarrier2 barrier(VkImage image, VkAccessFlags2 src, VkAccessFlags2 dst, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = src, b.dstAccessMask = dst, b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
void barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* b, uint32_t n) {
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = n, di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &di);
}

#ifndef _WIN32
/// (linux) a Vulkan FSR 4 build takes no frame without an exposure, and the chain's colour is tonemapped already: a 1x1 R32F of 1
bool unit_exposure(VkCommandBuffer cmd, TemporalImage& out) {
    static VkImage image = VK_NULL_HANDLE;
    static VmaAllocation allocation{};
    const bool made = image != VK_NULL_HANDLE;
    if (!made) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R32_SFLOAT, ci.extent = {1, 1, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!create_image(ci, image, allocation)) return image = VK_NULL_HANDLE, false;
    }
    out = TemporalImage{image, VK_NULL_HANDLE, VK_FORMAT_R32_SFLOAT, 1, 1};
    if (!made) {
        const VkImageMemoryBarrier2 to_clear = barrier(image, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barriers(cmd, &to_clear, 1);
        const VkClearColorValue one{{1.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
        const VkImageMemoryBarrier2 to_read = barrier(image, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barriers(cmd, &to_read, 1);
    }
    return true;
}
#endif

// ---- the engine (vk_temporal.h TemporalEngine) ----

bool start() { return load(); }

void release() {
    State& s = st();
    if (!s.context) return;
    std::shared_ptr<Context> old = std::move(s.context);
    temporal_later([old] {});   // (destroyed with the last copy, a few frames on: recorded dispatches may still use it)
    s.context.reset();
}

int prepare(VkCommandBuffer, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool /*native_aa*/,
            uint32_t* in_w, uint32_t* in_h, const char** why) {
    State& s = st();
    int made = 1;
    // (the game's native HDR: Context::hdr. What the game presents, not the option and the swapchain: with Native picked and
    // the game's picture not PQ, the scene is LDR. RX_VK_FSR4_HDR=0: the LDR context and the clamped colour with Native too)
    static const bool hdr_allowed = env_switch("RX_VK_FSR4_HDR") != 0;
    const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);
    if (!s.context || s.context->width != ow || s.context->height != oh || s.context->hdr != hdr) {
        if (s.context && s.context->hdr != hdr) logf("vk: AMD FSR 4: made again: the game's native HDR turned %s", hdr ? "on" : "off");
        release();
        s.context = make_context(ow, oh, hdr);
        if (!s.context) return *why = "its upscaler was not made", -1;
        s.in_w = s.in_h = 0;
        made = 2;
    }
    // (the inputs as large as the largest render size this context has had, never past the output: a fixed share keeps
    // images of its own size, and the game's dynamic resolution only makes them again when it goes past them)
    s.in_w = std::min(std::max(s.in_w, rw), ow), s.in_h = std::min(std::max(s.in_h, rh), oh);
    *in_w = s.in_w, *in_h = s.in_h;
    return made;
}

bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {
    State& s = st();
    Library& l = lib();
#ifndef _WIN32
    TemporalImage exposure{};
    if (!unit_exposure(cmd, exposure)) return false;
#endif
    const auto now = std::chrono::steady_clock::now();
    const float frame_ms = s.evals ? std::chrono::duration<float, std::milli>(now - s.last).count() : 1000.0f / 30.0f;
    s.last = now;

    constexpr VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkAccessFlags2 any = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    const VkImageMemoryBarrier2 in[4] = {barrier(f.color.image, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                                         barrier(f.depth.image, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                                         barrier(f.motion.image, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                                         barrier(f.output.image, any, any, G, G)};
    barriers(cmd, in, 4);

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resource(f.color, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(f.depth, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(f.motion, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(f.output, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
#ifndef _WIN32
    if (!s.context->hdr)   // (an HDR context works out its own exposure: FFX_UPSCALE_ENABLE_AUTO_EXPOSURE)
        d.exposure = resource(exposure, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
#endif
    d.jitterOffset = {f.jitter[0], f.jitter[1]};   // (render pixels, as the projection was offset: the same as DLSS's)
    d.motionVectorScale = {1.0f, 1.0f};             // (render pixels already, towards the frame before)
    d.renderSize = {f.render_w, f.render_h};
    d.upscaleSize = {f.output.width, f.output.height};
    d.enableSharpening = false;   // (the Sharpness row sharpens at the window, after the HUD)
    d.sharpness = 0.0f;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.near_z, d.cameraFar = f.far_z, d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = s.context->hdr ? 0u : (uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;   // (HDR: linear light)
    ffxReturnCode_t r;
    {
        GprofSpan timed(cmd, "fsr4 ffxDispatch (AMD's passes)");   // (RX_VK_PASS_TIMING=1: AMD's own GPU time, named)
        ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (ffxDispatch records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
        r = l.dispatch(&s.context->context, &d.header);
        note_work();   // (AMD's dispatches: full_barrier must not take the next barrier for a repeat of the one before them)
    }

    const VkImageMemoryBarrier2 out[4] = {barrier(f.color.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.depth.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.motion.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.output.image, any, any, G, G)};
    barriers(cmd, out, 4);
    if (r != FFX_API_RETURN_OK) {
        logf("vk: AMD FSR 4: its dispatch failed (%d)", (int)r);
        return false;
    }
    // (the first evaluation, and the first resets: a camera cut each)
    if (s.evals++ == 0 || (f.reset && s.told_resets < 40))
        s.told_resets++, logf("vk: AMD FSR 4: %ux%u -> %ux%u, jitter %+.3f %+.3f px%s", f.render_w, f.render_h, f.output.width,
                              f.output.height, f.jitter[0], f.jitter[1], f.reset ? " (history reset)" : "");
    return true;
}

uint64_t memory() {
    State& s = st();
    return s.context ? s.context->memory : 0;
}

/// Its context takes HDR colour (made for the game's native HDR): the chain gives it linear light, unclamped
bool hdr() {
    State& s = st();
    return s.context && s.context->hdr;
}

const TemporalEngine kEngine{"AMD FSR 4", "FSR", start, prepare, evaluate, release, memory, hdr};

}  // namespace

bool fsr4_built() { return true; }
const TemporalEngine* fsr4_engine() { return &kEngine; }

void fsr4_probe() {
    if (fsr4_why() != FSR_OK || g_why.load(std::memory_order_relaxed) >= 0 || !dev.device) return;
    load();
}

void fsr4_close() { tune::close(); }

bool fsr4_wants_descriptor_buffer() {
#ifdef _WIN32
    return false;
#else
    if (fsr4_why() == FSR_ENV_OFF) return false;
    for (const std::string& dir : library_dirs())
        if (access((dir + "/" + kLoader).c_str(), R_OK) == 0) return true;
    return false;
#endif
}

#else  // (no HOOKSHOT_FSR in this build: FSR is not there)

bool fsr4_built() { return false; }
const TemporalEngine* fsr4_engine() { return nullptr; }
void fsr4_probe() {}
void fsr4_close() {}
bool fsr4_wants_descriptor_buffer() { return false; }

#endif

}  // namespace mkwvk
