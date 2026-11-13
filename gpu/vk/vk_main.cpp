// The Vulkan back end's start: vulkan-1.dll, the instance, the device (Vulkan 1.3 and the extensions the NVN replay
// wants where the GPU has them: mesh shaders, host memory import, graphics pipeline libraries), the window's
// swapchain, and the rx_* table (vk_api.h). The NVN replay itself is vk_replay.cpp.
#include "vk_paths.h"
#include "vk_loader.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>   // (sysconf: the RAM, auto_profile)
#endif

#include "vk_api.h"
#include "vk_draw.h"
#include "vk_framegen.h"
#include "vk_internal.h"
#include "presenter_queue.h"
static_assert(mkwvk::kQueueGraphics == VK_QUEUE_GRAPHICS_BIT && mkwvk::kQueueCompute == VK_QUEUE_COMPUTE_BIT);
#include "gpu_choice.h"
#include "vk_menu.h"
#include "auto_settings.h"
#include "vk_dlss.h"
#include "vk_fsr4.h"
#include "vk_reflex.h"
#include "vk_rt.h"
#include "vk_antilag.h"
#include "vk_progress.h"
#include "vk_programs.h"
#include "mesh_mode.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace vkx {
#define VKX_DEFINE(f) PFN_##f f;
VKX_GLOBAL(VKX_DEFINE)
VKX_INSTANCE(VKX_DEFINE)
VKX_DEVICE(VKX_DEFINE)
VKX_DEVICE2(VKX_DEFINE)
VKX_DEVICE3(VKX_DEFINE)
#undef VKX_DEFINE

static PFN_vkGetInstanceProcAddr get_instance_proc;

bool load_global() {
#ifdef _WIN32   // (hookshot: the loader from System32 only, not from the current folder or PATH)
    HMODULE m = LoadLibraryExA("vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    HMODULE m = LoadLibraryA("vulkan-1.dll");   // (linux: libvulkan.so.1, host/posix/win32_posix.c)
#endif
    if (!m) return false;
    get_instance_proc = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(m, "vkGetInstanceProcAddr");
    if (!get_instance_proc) return false;
#define VKX_LOAD(f) f = (PFN_##f)get_instance_proc(nullptr, #f);
    VKX_GLOBAL(VKX_LOAD)
#undef VKX_LOAD
    return vkCreateInstance != nullptr;
}
PFN_vkGetInstanceProcAddr instance_proc_addr() { return get_instance_proc; }
void load_instance(VkInstance instance) {
#define VKX_LOAD(f) f = (PFN_##f)get_instance_proc(instance, #f);
    VKX_INSTANCE(VKX_LOAD)
#undef VKX_LOAD
}
void load_device(VkDevice device) {
#define VKX_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(device, #f);
    VKX_DEVICE(VKX_LOAD)
    VKX_DEVICE2(VKX_LOAD)
    VKX_DEVICE3(VKX_LOAD)
#undef VKX_LOAD
}
} // namespace vkx

namespace mkwvk {

int display_refresh_mhz();   // (below, outside the unnamed namespace)

std::vector<std::string> gpu_device_names;
const MkwVkHost* host;
int64_t prof_ns[PROF_COUNT];
Device dev;
PFN_vkGetInstanceProcAddr instance_proc() { return vkx::instance_proc_addr(); }
/// (hookshot) The driver as it names itself (VkPhysicalDeviceDriverProperties: "NVIDIA 580.95.05", "radv Mesa 25.2.4"), else its
/// version number: the log's "vk: driver:" line and the menu's Save my pipeline list (vk_menu.cpp)
std::string driver_text() {
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    if (dev.physical) vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    std::string name(drv.driverName, strnlen(drv.driverName, sizeof drv.driverName)),
        info(drv.driverInfo, strnlen(drv.driverInfo, sizeof drv.driverInfo));
    if (name.empty() && info.empty()) {
        char b[32];
        snprintf(b, sizeof b, "%#x", dev.props.driverVersion);
        return b;
    }
    return info.empty() ? name : name.empty() ? info : name + " " + info;
}

/// (hookshot) Milliseconds on the steady clock: the log's last line and the preparation's watchdog
int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int64_t> last_log_ms{0};   // (when logf last wrote: the preparation's status line comes after a silence)
void logf(const char* fmt, ...) {
    char b[1024];
    std::va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    last_log_ms.store(steady_ms(), std::memory_order_relaxed);
    if (host) host->log(b);
}

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

bool create_instance() {
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        logf("vk: the Vulkan loader is %u.%u: 1.3 is needed", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version));
        return false;
    }
#ifdef _WIN32
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const uint32_t n_exts = 2;
#else
    // (linux: every window system's surface the loader has, for whichever one SDL's window is on: X11 or Wayland)
    const char* exts[4] = {VK_KHR_SURFACE_EXTENSION_NAME};
    uint32_t n_exts = 1;
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
        for (const char* want : {"VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface"})
            if (has_extension(all, want)) exts[n_exts++] = want;
    }
#endif
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mkw-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    std::vector<const char*> ext_list(exts, exts + n_exts);
    {   // (hookshot) what NVIDIA DLSS needs (vk_dlss.cpp; nothing in a build without it)
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
        dlss_instance_extensions(ext_list, all);
        // (hookshot) the HDR10 colour space for the Insert menu's HDR (the menu's settings are read after the instance is made, so
        // whenever the driver has it; the SDR choice in create_swapchain keeps to the sRGB surface)
        if (has_extension(all, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            ext_list.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME), dev.swapchain_colorspace = true;
    }
    ci.enabledExtensionCount = (uint32_t)ext_list.size();
    ci.ppEnabledExtensionNames = ext_list.data();
    // RX_VK_VALIDATION=1: the Khronos validation layer (when installed)
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (env_on("RX_VK_VALIDATION")) ci.enabledLayerCount = 1, ci.ppEnabledLayerNames = &layer;
    if (vkCreateInstance(&ci, nullptr, &dev.instance) != VK_SUCCESS) {
        logf("vk: vkCreateInstance failed");
        return false;
    }
    load_instance(dev.instance);
    return true;
}

bool pick_device() {
    menu_init();
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(dev.instance, &n, all.data());

    std::vector<uint32_t> valid_gpus;
    gpu_device_names.clear();
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        logf("vk: device %u: %s, Vulkan %u.%u, driver %#x, type %d", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion, (int)p.deviceType);
        if (p.apiVersion >= VK_API_VERSION_1_3) {
            valid_gpus.push_back(i);
            gpu_device_names.push_back(p.deviceName);
        }
    }
    menu_set_gpu_count((int)valid_gpus.size());

    // (AMD laptops) With AMD graphics in the processor and an AMD card, Windows has two AMD drivers installed, one an
    // adapter, and each one's Vulkan driver lists every AMD GPU: the card shows up twice. It came first through the
    // processor graphics' driver, which the card's own update leaves as it was: on an RX 7700S that older driver
    // (amdvlk64.dll 32.0.11036.4002) crashed in its shader compiler (0xc0000409) on the game's compute pipelines while
    // the card's 32.0.32015.7029 built them. The same kind of GPU twice: the one with the newer driver wins.
    // RX_VK_DEVICE=<n> or opts.gpu_device: that device, as listed in the log
    int best = -1, best_score = -1;
    uint32_t best_driver = 0;
    int target_raw_idx = -1;
    if (const char* want = getenv("RX_VK_DEVICE")) {
        if (*want) target_raw_idx = atoi(want);
    } else if (const int k = gpu_choice::menu_device(opts.gpu_device, (int)valid_gpus.size()); k >= 0) {
        target_raw_idx = (int)valid_gpus[k];
    }

    for (uint32_t i : valid_gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        if (target_raw_idx >= 0) {
            if ((int)i == target_raw_idx) best = (int)i, best_score = 3;
            continue;
        }
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                          p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > best_score || (score == best_score && p.driverVersion > best_driver))
            best = (int)i, best_score = score, best_driver = p.driverVersion;
    }
    if (best < 0 && target_raw_idx >= 0) {
        logf("vk: preferred device %d not available or lacks Vulkan 1.3, falling back to auto selection", target_raw_idx);
        for (uint32_t i : valid_gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(all[i], &p);
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                              p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score || (score == best_score && p.driverVersion > best_driver))
                best = (int)i, best_score = score, best_driver = p.driverVersion;
        }
    }
    if (best < 0) {
        logf("vk: no Vulkan 1.3 GPU");
        return false;
    }
    dev.physical = all[best];
    vkGetPhysicalDeviceProperties(dev.physical, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &dev.memory);
    return true;
}

/// (hookshot) Whether the shaders' float adds to storage buffers are asked to be the GPU's own atomic (create_device):
/// RX_VK_NATIVE_FADD by the switches' rule (hookshot_env.h env_value_off: 0, false, off, no or empty is off, any other
/// value on); unset, only with NVIDIA's driver, the one the native add was tried on in the game (an RTX 4090 in Kakariko
/// and on Death Mountain Trail). AMD's RDNA 3 and later and Intel's Arc report the feature too but were not tried: they
/// keep the loop unless the switch asks, until runs on them say otherwise
static bool native_fadd_wanted(const char* env, VkDriverId driver) {
    if (env) return !env_value_off(env);
    return driver == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
}

bool create_device() {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, exts.data());
    std::vector<const char*> want = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto optional = [&](const char* name, bool& flag) {
        flag = has_extension(exts, name);
        if (flag) want.push_back(name);
    };
    // (#1001: how task and mesh programs run, mesh_mode.h: RX_VK_MESH_SHADERS=native or compute, a launch switch; unset,
    // Automatic. RX_VK_FORCE_MESH_FALLBACK=1, a test, is Compute over it: mesh shaders not used where they are, so the no-mesh
    // fallback the GTX 10xx and RX 5000 cards run (issue 640) can be tried on any GPU. The Graphics tab's row it was, and its
    // mesh_shaders= in hookshot_options.ini, no longer choose: the line is only told of in the log)
    static const mesh_rule::Choice mesh_choice = [] {
        char exe[MAX_PATH];
        const DWORD len = game_module_path(exe, MAX_PATH);   // (the game folder, not bin beside the launcher: vk_paths.h)
        std::string ini(exe, len);
        ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
        if (FILE* fl = fopen(ini.c_str(), "r")) {
            char line[80];
            int old = 0;
            while (fgets(line, sizeof line, fl))
                if (mesh_rule::old_ini_choice(line, &old)) {
                    logf("vk: mesh_shaders=%d in hookshot_options.ini is no longer read (the Mesh shaders row left the menu): "
                         "set RX_VK_MESH_SHADERS=native or compute in hookshot_env.txt to choose", old);
                    break;
                }
            fclose(fl);
        }
        return mesh_rule::choose(getenv("RX_VK_MESH_SHADERS"), getenv("RX_VK_FORCE_MESH_FALLBACK"));
    }();
    const int mesh_mode = mesh_choice.mode;
    // (hookshot, issue 2245) RADV's native task/mesh path deadlocks the GPU: the gfx ring and RADV's gang compute ring
    // end up waiting on each other, and a mesh-only draw hangs with the compute queue empty (devcoredumps on an
    // RX 6800: no VM fault; waves stuck on gfx in the second). Task + mesh draws alone on the fallback still crash, so
    // only full Compute is stable there, and the reports span Navi 2x (RX 6800, RX 6600 XT) and the RDNA3 APUs (Steam
    // Deck OLED, Rog Xbox Ally X). On Linux, where AMD's Vulkan driver is RADV, Automatic picks Compute; Native stays
    // a choice (RX_VK_MESH_SHADERS=native). RDNA1 and older have no mesh shaders on RADV: for them a no-op.
    // (the card's own, before the mode below: whether it has task and mesh shaders, for the Quality preset Auto (this PC), which
    // must not put a card that has them on Minimum because this run uses Compute)
    if (has_extension(exts, VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        VkPhysicalDeviceMeshShaderFeaturesEXT m{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m};
        vkGetPhysicalDeviceFeatures2(dev.physical, &f2);
        dev.mesh_capable = m.meshShader && m.taskShader;
    }
    int mesh_mode_used = mesh_mode;
#ifndef _WIN32
    if (mesh_mode_used == 0 && dev.props.vendorID == 0x1002) mesh_mode_used = 2;   // (Linux AMD is RADV)
#endif
    if (mesh_mode_used == 2) dev.mesh_shader = false;
    else optional(VK_EXT_MESH_SHADER_EXTENSION_NAME, dev.mesh_shader);
    optional(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, dev.external_memory_host);
    optional(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, dev.external_memory_fd);   // (guest memory as udmabufs: vk_memory.cpp import_dma_buf)
    if (dev.external_memory_fd) optional(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, dev.dma_buf_import);
    optional(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, dev.pipeline_library);
    bool module_id = false;   // (not a field of dev: vk_state.h is part of the translator's stamp)
    optional(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME, module_id);
    if (dev.pipeline_library) optional(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, dev.graphics_pipeline_library);
    optional(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, dev.custom_border_color);
    optional(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, dev.dynamic_state3);
    optional(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, dev.depth_clip_control);
    optional(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, dev.null_descriptor);
    optional(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, dev.index_uint8);
    optional(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, dev.push_descriptor);
    optional(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, dev.conditional_rendering);
    optional(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, dev.provoking_vertex);
    optional(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, dev.vertex_divisor);
    optional(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, dev.stencil_export);
    optional(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, dev.memory_budget);
    optional(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME, dev.view_2d_of_3d);
    // (hookshot) on by default: with a marker before each draw and dispatch the device has not been lost (five
    // runs); without them it was, every run, shortly after OoT's first gameplay frames (nvlddmkm event 153; the
    // extension alone, RX_VK_CHECKPOINTS=ext, did not keep it). The race they hide is not found yet.
    // RX_VK_CHECKPOINTS=0: off
    if (!getenv("RX_VK_CHECKPOINTS") || strcmp(getenv("RX_VK_CHECKPOINTS"), "0") != 0)
        optional(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, dev.checkpoints);
    // (asked for only: a marker written twice a draw, where the NV ones are not there to hide a race)
    if (const char* c = getenv("RX_VK_CHECKPOINTS"); !dev.checkpoints && c && strcmp(c, "0") != 0 && strcmp(c, "ext") != 0)
        optional(VK_AMD_BUFFER_MARKER_EXTENSION_NAME, dev.buffer_marker);
    optional(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, dev.device_fault);
    optional(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, dev.coop_matrix);
    // (hookshot) the GPU's own float add on storage buffers: the game's RED/ATOM.ADD.F32 become one OpAtomicFAddEXT instead
    // of a compare-exchange loop, which gives up after CAS_LOOP_MAX_TRIES attempts and loses that add (eden
    // spirv_emit_context.cpp DefineStorageBuffers). By default with NVIDIA's driver only (native_fadd_wanted);
    // RX_VK_NATIVE_FADD=1 asks for it on any GPU, =0 keeps the loop on every one, the extension not even enabled
    const char* const fadd_env = getenv("RX_VK_NATIVE_FADD");
    VkPhysicalDeviceDriverProperties fadd_driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    {
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &fadd_driver;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    }
    const bool fadd_wanted = native_fadd_wanted(fadd_env, fadd_driver.driverID);
    bool atomic_float = false;
    if (fadd_wanted) optional(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, atomic_float);
    optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, dev.swap_mutable);   // (hookshot: the overlays, vk_imgui.cpp)
    dlss_device_extensions(want, exts);   // (hookshot: NVIDIA DLSS's, vk_dlss.cpp)
    dlss_fg_device_extensions(want, exts);   // (hookshot: NVIDIA DLSS Frame Generation's through NGX, asked of NGX)
    bool fsr_descriptor_buffer = false;   // (hookshot, linux: only with a Vulkan FSR 4 library beside the game, vk_fsr4.cpp)
    if (fsr4_wants_descriptor_buffer()) optional(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, fsr_descriptor_buffer);
    reflex_device_extensions(want, exts);   // (hookshot: NVIDIA Reflex's, vk_reflex.cpp)
    rt_device_extensions(want, exts);   // (miojo: ray queries, vk_rt.cpp; RX_VK_RT=1)
    antilag_device_extensions(want, exts);   // (hookshot: AMD Anti-Lag 2's, vk_antilag.cpp)

    // the queue: one family for graphics, compute and transfer
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, families.data());
    dev.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < n && dev.queue_family == UINT32_MAX; i++)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            dev.queue_family = i;
    if (dev.queue_family == UINT32_MAX) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = dev.queue_family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    // (hookshot) The presents on a queue of a compute family of their own, beside the replay's (RX_VK_PRESENT_QUEUE=1 / 0: on /
    // off; on by default with NVIDIA's driver on Windows, where it was measured). On the replay's queue vkQueuePresentKHR blocked
    // with dev.present_mu held, ~0.8 ms a present with frame interpolation at 4x and up to 8, and the game's main and presentation
    // threads waited there to submit. A second queue of the graphics family took that lock off the presents, but its presents
    // blocked longer (~2-4 ms: the graphics work of the two queues takes turns on the GPU) and the made frames were shown late
    // four times as often. A compute family's queue runs beside the graphics work: in Hyrule Field (a 14700K and an RTX 5070 Ti,
    // 4x, 175 Hz) the game went from ~52 to ~58 fps standing and ~46.5 to ~53 running, its frames over 22 ms from ~13% to ~1%
    // standing and over 40 ms from ~4% to ~0.2% running, the made frames as evenly shown standing. The swapchain's images are
    // shared by both families (vk_main.cpp create_swapchain), and a family that cannot present to the window leaves it off.
    uint32_t compute_family = UINT32_MAX;
    {
        bool want = false;
#ifdef _WIN32
        VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        want = false;   // (v2.0.9: off by default with the write-back diff until issue 1161's device lost is placed; was:
        (void)dp;       //  NVIDIA's driver on Windows. RX_VK_PRESENT_QUEUE=1: on)
#endif
        if (const char* e = getenv("RX_VK_PRESENT_QUEUE")) want = atoi(e) != 0;
        for (uint32_t i = 0; want && i < n && compute_family == UINT32_MAX; i++)
            if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                compute_family = i;
    }
    // (hookshot) The presenter thread's own queue (vk_replay.cpp; RX_VK_PRESENT_THREAD=0: none): a second queue of the replay's
    // family (NVIDIA's has 16), else a queue of a compute-only family (RADV: one graphics queue), its family's next free one. A
    // device without either presents on the game's thread, as before
    const char* pt_env = getenv("RX_VK_PRESENT_THREAD");
    const bool presenter_wanted = pt_env && atoi(pt_env) != 0;   // (off by default: no queue made for it)
    uint32_t presenter_family = UINT32_MAX, presenter_index = 0;
    if (presenter_wanted) {   // (presenter_queue.h: never a queue another part of the renderer takes)
        std::vector<QueueFamilyInfo> qf(n);
        for (uint32_t i = 0; i < n; i++) qf[i] = {(uint32_t)families[i].queueFlags, families[i].queueCount};
        if (!presenter_queue_pick(qf.data(), n, dev.queue_family, compute_family, presenter_family, presenter_index))
            presenter_family = UINT32_MAX;
    }
    static const float priorities[2] = {1.0f, 1.0f};
    std::vector<VkDeviceQueueCreateInfo> qs;
    auto queues = [&](uint32_t family, uint32_t count) {
        for (auto& x : qs)
            if (x.queueFamilyIndex == family) { x.queueCount = std::max(x.queueCount, count); return; }
        VkDeviceQueueCreateInfo x{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        x.queueFamilyIndex = family, x.queueCount = count, x.pQueuePriorities = priorities;
        qs.push_back(x);
    };
    queues(dev.queue_family, 1);
    if (compute_family != UINT32_MAX) queues(compute_family, 1);
    if (presenter_family != UINT32_MAX) queues(presenter_family, presenter_index + 1);
    (void)q;
    // (and the presenter's waits for its presents to be shown: VK_KHR_present_id + VK_KHR_present_wait, where the driver has them)
    const bool want_present_wait = presenter_family != UINT32_MAX && has_extension(exts, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                                   has_extension(exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (want_present_wait)
        for (const char* name : {VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_EXTENSION_NAME}) {
            bool listed = false;
            for (const char* w : want) listed |= !strcmp(w, name);
            if (!listed) want.push_back(name);
        }

    // features: what the hardware has of what the replay uses (all of them on the GPUs MKW-class hardware maps to)
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    void** tail = &f13.pNext;
    // (hookshot: the load at a program's create call makes a pipeline from the caches by its module's identifier, without the
    // program's code: vk_programs.h. Nothing else uses it)
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT module_idf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT};
    if (module_id) *tail = &module_idf, tail = &module_idf.pNext;
    if (dev.mesh_shader) *tail = &mesh, tail = &mesh.pNext;
    if (dev.null_descriptor) *tail = &robust2, tail = &robust2.pNext;
    if (dev.index_uint8) *tail = &u8, tail = &u8.pNext;
    if (dev.dynamic_state3) *tail = &eds3, tail = &eds3.pNext;
    if (dev.custom_border_color) *tail = &border, tail = &border.pNext;
    if (dev.depth_clip_control) *tail = &clip, tail = &clip.pNext;
    if (dev.conditional_rendering) *tail = &cond, tail = &cond.pNext;
    if (dev.provoking_vertex) *tail = &pv, tail = &pv.pNext;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdiv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    if (dev.vertex_divisor) *tail = &vdiv, tail = &vdiv.pNext;
    if (dev.graphics_pipeline_library) *tail = &gpl, tail = &gpl.pNext;
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (dev.device_fault) *tail = &fault, tail = &fault.pNext;
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v2d3d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT};
    if (dev.view_2d_of_3d) *tail = &v2d3d, tail = &v2d3d.pNext;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    if (dev.coop_matrix) *tail = &coop, tail = &coop.pNext;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT afloat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    if (atomic_float) *tail = &afloat, tail = &afloat.pNext;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    if (fsr_descriptor_buffer) *tail = &descriptor_buffer, tail = &descriptor_buffer.pNext;
    reflex_device_features(tail);   // (present ids, when NVIDIA Reflex's extensions are there)
    rt_device_features(tail);
    VkPhysicalDevicePresentIdFeaturesKHR present_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    if (want_present_wait) {
        if (!reflex_present_id_features()) *tail = &present_id, tail = &present_id.pNext;   // (one of each struct in the chain)
        *tail = &present_wait, tail = &present_wait.pNext;
    }
    antilag_device_features(tail);   // (AMD Anti-Lag 2's, when its extension is there)
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    if (module_id && module_idf.shaderModuleIdentifier) module_ids_on();
    descriptor_buffer.descriptorBufferCaptureReplay = VK_FALSE;
    dev.view_2d_of_3d = dev.view_2d_of_3d && v2d3d.sampler2DViewOf3D;
    coop.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    dev.coop_matrix = dev.coop_matrix && coop.cooperativeMatrix &&
                      f12.vulkanMemoryModel && f12.vulkanMemoryModelDeviceScope;
    {   // (only the float add on buffers on: the one the translator emits)
        dev.float_atomic_add = atomic_float && afloat.shaderBufferFloat32AtomicAdd;
        void* const next = afloat.pNext;
        afloat = VkPhysicalDeviceShaderAtomicFloatFeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
        afloat.pNext = next;
        afloat.shaderBufferFloat32AtomicAdd = dev.float_atomic_add ? VK_TRUE : VK_FALSE;
        const char* const how = dev.float_atomic_add ? "native (VK_EXT_shader_atomic_float)"
                                : !fadd_wanted       ? "compare-exchange loop"
                                : atomic_float       ? "compare-exchange loop (no shaderBufferFloat32AtomicAdd)"
                                                     : "compare-exchange loop (no VK_EXT_shader_atomic_float)";
        if (fadd_env) logf("vk: float atomic add on storage buffers: %s, RX_VK_NATIVE_FADD=%s", how, fadd_env);
        else logf("vk: float atomic add on storage buffers: %s, %s", how,
                  fadd_wanted ? "NVIDIA's driver" : "by default only with NVIDIA's driver (RX_VK_NATIVE_FADD=1: native where the GPU has it)");
    }
    // (bounds-checked buffer access: an out-of-range access dropped instead of faulting the device. On by default: the GPU
    // culling pass 53caac5f faulted the device while a save loaded, about one start in three, and the scene's lighting read garbage
    // for single frames (the black flashes); with it, neither in A/B runs at the same GPU time. Lichii's OoT branch keeps it on too.
    // The 'Robust buffers' row of hookshot_options.ini chooses (0 Auto: on, on every vendor; 1 Off; 2 On); RX_VK_ROBUST=0 or 1
    // over it. Off is null descriptors only: some NVIDIA cards ran slower with it on, issues 394, 418, 419)
    static const bool robust = []() -> bool {
        if (const char* e = getenv("RX_VK_ROBUST")) return atoi(e) != 0;
        char exe[MAX_PATH];
        const DWORD n = game_module_path(exe, MAX_PATH);   // (the game folder, not bin beside the launcher: vk_paths.h)
        std::string ini(exe, n);
        ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
        int v = 0;   // (Auto)
        if (FILE* f = fopen(ini.c_str(), "r")) {
            char line[80];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "robust_buffers=", 15)) {
                    v = atoi(line + 15);
                    break;
                }
            fclose(f);   // (once: the PR closed it twice when the row said Auto)
        }
        return v != 1;   // (Auto and On: on)
    }();
    const bool robust_ok = robust && robust2.robustBufferAccess2 && f.features.robustBufferAccess;
    robust2.robustBufferAccess2 = robust_ok ? VK_TRUE : VK_FALSE;
    robust2.robustImageAccess2 = VK_FALSE;
    dev.null_descriptor = dev.null_descriptor && robust2.nullDescriptor;
    pv.transformFeedbackPreservesProvokingVertex = VK_FALSE;
    dev.provoking_vertex = dev.provoking_vertex && pv.provokingVertexLast;
    dev.conditional_rendering = dev.conditional_rendering && cond.conditionalRendering;
    dev.custom_border_color = dev.custom_border_color && border.customBorderColors && border.customBorderColorWithoutFormat;
    dev.depth_clip_control = dev.depth_clip_control && clip.depthClipControl;
    dev.graphics_pipeline_library = dev.graphics_pipeline_library && gpl.graphicsPipelineLibrary;
    if (dev.graphics_pipeline_library) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT lp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &lp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.graphics_pipeline_library = lp.graphicsPipelineLibraryFastLinking != VK_FALSE;
    }
    dev.eds3_polygon = dev.dynamic_state3 && eds3.extendedDynamicState3PolygonMode;
    dev.eds3_clamp = dev.dynamic_state3 && eds3.extendedDynamicState3DepthClampEnable;
    dev.eds3_blend = dev.dynamic_state3 && eds3.extendedDynamicState3ColorBlendEnable &&
                     eds3.extendedDynamicState3ColorBlendEquation && eds3.extendedDynamicState3ColorWriteMask;
    {   // (hookshot) the vendor and driver workarounds this device needs: vk_vendor.h, each forced by its RX_VK_ switch
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        VendorFacts facts;
        facts.vendor_id = p2.properties.vendorID;
        facts.driver_id = drv.driverID;
        facts.device_type = p2.properties.deviceType;
        facts.driver_info = drv.driverInfo;
        dev.fixes = vendor_fixes(facts);
        vendor_fixes_env(dev.fixes, [](const char* name) -> const char* { return getenv(name); });
        logf("vk: vendor fixes for %s (%s): out-of-bounds texel fetches 0 %d, NaN tests by bits %d, gather offset %d",
             drv.driverName, drv.driverInfo, (int)dev.fixes.fetch_bounds, (int)dev.fixes.nan_bits, (int)dev.fixes.gather_offset);
    }
    dev.eds3_a2c = dev.dynamic_state3 && eds3.extendedDynamicState3AlphaToCoverageEnable;
    dev.vk12 = f12;
    dev.vk13 = f13;
    {   // (hookshot, after TotK Recomp's vk_caps.c) NVN's programs are written for 32 lanes: the translator is told 32, and the lane
        // shuffles of OoT's networks and the lane masks of its mesh programs depend on it. NVIDIA's GPUs have no other size; AMD's
        // default to 64 where they like, and the networks' output came out as coloured noise (Hookshot issues 10 and 14)
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        sc.pNext = &drv;
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sg.pNext = &sc;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        const bool pin = f13.subgroupSizeControl && sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 && !env_on("RX_VK_NO_SUBGROUP_PIN");
        dev.sg32_stages = pin ? sc.requiredSubgroupSizeStages : 0;
        dev.subgroup_size = sg.subgroupSize;
        dev.subgroup_max = sc.maxSubgroupSize > sg.subgroupSize ? sc.maxSubgroupSize : sg.subgroupSize;
        dev.driver_id = drv.driverID;
        // (RADV on GCN 1-4, GFX6-8: no 16-bit shader inputs and outputs there, which RADV has from GFX9 (packed 16-bit math) on.
        // Its compiler cannot build the biggest mesh fallback programs: fallback_size_policy.h)
        dev.radv_gcn = drv.driverID == VK_DRIVER_ID_MESA_RADV && !f11.storageInputOutput16;
        logf("vk: subgroups: default %u lanes, %u-%u, pinned to 32 on stages %#x; RADV on GCN 1-4: %s", sg.subgroupSize, sc.minSubgroupSize,
             sc.maxSubgroupSize, (unsigned)dev.sg32_stages, dev.radv_gcn ? "yes" : "no");
    }
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
        logf("vk: the GPU lacks dynamic rendering, synchronization2 or timeline semaphores");
        return false;
    }
    mesh.multiviewMeshShader = mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;   // (not used)
    dev.mesh_shader = dev.mesh_shader && mesh.meshShader && mesh.taskShader;
    // (task and mesh shader counts in pipeline statistics, a feature of their own: RADV has it off on RDNA2, and a query
    // pool made with those counts all the same can hang the GPU)
    dev.mesh_queries = dev.mesh_shader && mesh.meshShaderQueries;
    // (hookshot, issue 640) RDNA1 and older GPUs: no mesh shaders, but task and mesh programs run as compute shaders
    // that write their vertices to buffers, drawn through an ordinary graphics pipeline. It needs buffer device addresses.
    dev.mesh_fallback = !dev.mesh_shader && f12.bufferDeviceAddress && !env_on("RX_VK_NO_MESH_FALLBACK");
    if (dev.mesh_fallback) logf("vk: no mesh shaders: task and mesh programs run as compute shaders (issue 640 fallback)");
    bool task_mesh_fallback = false;
#ifdef _WIN32
    // Navi 22 can time out on task + mesh draws in the AMD Windows driver (issue 996), and so can Navi 21: on an RX 6800 XT
    // (driver 0x800161) the native path lost the device within minutes at Hyrule Castle's entrance, while the fallback played
    // 25 minutes there with none. Navi 21 is 0x73bf (RX 6800, 6800 XT, 6900 XT), 0x73af (RX 6900 XT, XTXH boards) and 0x73a5
    // (RX 6950 XT): the same chip on the same driver, not tested on their own. Navi 22 is 0x73df (RX 6700 XT)
    task_mesh_fallback = dev.props.vendorID == 0x1002 &&
                         (dev.props.deviceID == 0x73bf || dev.props.deviceID == 0x73af || dev.props.deviceID == 0x73a5 ||
                          dev.props.deviceID == 0x73df);
#endif
    if (mesh_mode == 1) task_mesh_fallback = false;   // (Native: the card's own mesh shaders for every draw)
    if (const char* e = getenv("RX_VK_TASK_MESH_FALLBACK")) task_mesh_fallback = atoi(e) != 0;
    dev.task_mesh_fallback = task_mesh_fallback && dev.mesh_shader && f12.bufferDeviceAddress && !env_on("RX_VK_NO_MESH_FALLBACK");
    if (dev.task_mesh_fallback) logf("vk: task + mesh programs use the compute fallback (RX_VK_TASK_MESH_FALLBACK=0: native)");
    logf("vk: mesh shaders: %s, from %s: %s", mesh_mode == 2 ? "Compute" : mesh_mode == 1 ? "Native" : "Automatic",
         mesh_choice.why, dev.mesh_shader ? (dev.task_mesh_fallback ? "native, task + mesh draws on the compute path" : "native")
                         : dev.mesh_fallback ? "the compute path" : "none");
    if (robust) logf("vk: robust buffer access %s", robust_ok ? "on" : "not supported");
    f.features.robustBufferAccess = robust_ok ? VK_TRUE : VK_FALSE;   // (off by default: the game's accesses stay in their buffers)
    dev.features = f.features;
    dev.sparse_images = (families[dev.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f.features.sparseBinding &&
                        f.features.sparseResidencyImage2D && !(getenv("RX_VK_SPARSE") && !atoi(getenv("RX_VK_SPARSE")));
    dev.sparse_buffers = (families[dev.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f.features.sparseBinding;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f;
    ci.queueCreateInfoCount = (uint32_t)qs.size();
    ci.pQueueCreateInfos = qs.data();
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.ppEnabledExtensionNames = want.data();
    if (vkCreateDevice(dev.physical, &ci, nullptr, &dev.device) != VK_SUCCESS) {
        logf("vk: vkCreateDevice failed");
        return false;
    }


    load_device(dev.device);
    // (hookshot: when programs are loaded, vk_programs.h. Without mesh shaders, or with the task + mesh draws through the
    // fallback, a start keeps the fallback's programs ready, and loads at create have not been shown to be as good there)
    programs_load_decide(dev.mesh_shader && !dev.mesh_fallback && !dev.task_mesh_fallback);
    vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);
    dev.present_queue = VK_NULL_HANDLE;
    dev.present_family = dev.queue_family;
    if (compute_family != UINT32_MAX) vkGetDeviceQueue(dev.device, compute_family, 0, &dev.present_queue), dev.present_family = compute_family;
    dev.presenter_queue = VK_NULL_HANDLE;
    if (presenter_family != UINT32_MAX) {
        vkGetDeviceQueue(dev.device, presenter_family, presenter_index, &dev.presenter_queue), dev.presenter_family = presenter_family;
        const VkPhysicalDevicePresentIdFeaturesKHR* pid = reflex_present_id_features();
        dev.present_wait = want_present_wait && (pid ? pid->presentId : present_id.presentId) && present_wait.presentWait;
        logf("vk: the presenter thread's queue: family %u, queue %u%s; waits for its presents to be shown: %s", presenter_family,
             presenter_index, presenter_family == dev.queue_family ? " (the replay's family)" : " (a compute family)",
             dev.present_wait ? "yes (VK_KHR_present_wait)" : "no");
    } else if (presenter_wanted) {
        logf("vk: no second queue for the presenter thread: presents on the game's thread");
    }
    if (dev.buffer_marker) buffer_markers_init();
    coopmat_init();
    reflex_device_created();
    rt_device_created();
    antilag_device_created();
    if (dev.push_descriptor) {
        VkPhysicalDevicePushDescriptorPropertiesKHR pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.max_push_descriptors = pp.maxPushDescriptors;
    }
    return true;
}

} // namespace

std::string device_report_text() {
    if (!dev.physical) return "";
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    // (driverName and driverInfo: "NVIDIA" "580.95.05", "radv" "Mesa 25.2.4", "Intel Corporation" "101.6130")
    std::string text = std::string(p2.properties.deviceName) + ", " + drv.driverName;
    if (drv.driverInfo[0]) text += std::string(" ") + drv.driverInfo;
    return text;
}
/// (hookshot) The UNORM format of an sRGB one (the same bytes, read and written as they are), else the format itself
VkFormat unorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
/// (hookshot) The views of the swapchain's images let go (before the swapchain they view: the device idle already)
void swap_views_drop() {
    for (auto view : dev.swap_views)
        if (view) vkDestroyImageView(dev.device, view, nullptr);
    dev.swap_views.clear();
}
/// (hookshot) A view of each of the swapchain's images, for Dear ImGui's overlays (vk_imgui.cpp), when they can be drawn into,
/// in the format given (the swapchain's, or its UNORM one when the swapchain was made mutable)
void swap_views_make(VkImageUsageFlags usage, VkFormat format) {
    swap_views_drop();
    dev.swap_views.assign(dev.swap_images.size(), VK_NULL_HANDLE);
    dev.swap_view_format = format;
    if (!(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return;
    for (size_t i = 0; i < dev.swap_images.size(); i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = dev.swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev.device, &vi, nullptr, &dev.swap_views[i]) != VK_SUCCESS) dev.swap_views[i] = VK_NULL_HANDLE;
    }
}

/// (hookshot) Frame generation as the swapchain is made for it: the fixed multiplier, 2 to 4 (more takes no more images), or 0
/// for off and Auto. Its present mode under V-Sync Auto and its spare images follow it; present_image makes the swapchain
/// again when it changes
int swapchain_framegen() { return opts.framegen >= 2 ? std::min(opts.framegen, 4) : 0; }

/// (hookshot) Whether the swapchain is made for NVIDIA DLSS Frame Generation: a fixed multiplier, and DLSS-G making the sets
/// (vk_dlss.cpp dlss_fg_runs: asked for, on a device it runs on, its runtime making as many frames as the multiplier asks).
/// V-Sync Auto is FIFO for it, where it was measured; the renderer's own methods and other GPUs keep Auto's mailbox (or
/// immediate), and so does DLSS-G asked for more frames than its runtime makes, which the renderer's own then makes.
/// present_image makes the swapchain again when it changes: NGX's answer comes at the first present with a multiplier, so a
/// swapchain made for DLSS-G before it is made once more without. (The set's made frames are the row's multiplier less 1, up
/// to 8x: swapchain_framegen stops at 4, for the images)
bool swapchain_dlssg() { return swapchain_framegen() >= 2 && dlss_fg_runs(opts.framegen - 1); }

void retire_swapchain() {
    if (!dev.swapchain) return;
    device_wait_idle_locked();
    reflex_swapchain_destroyed();
    swap_views_drop();
    for (auto semaphore : dev.swap_rendered) vkDestroySemaphore(dev.device, semaphore, nullptr);
    vkDestroySwapchainKHR(dev.device, dev.swapchain, nullptr);
    dev.swapchain = VK_NULL_HANDLE;
    ++dev.swapchain_generation;
    dev.swap_rendered.clear();
    dev.swap_images.clear();
    dev.swap_spare = 0;
}

bool invalidate_lost_surface(VkResult result) {
    if (result != VK_ERROR_SURFACE_LOST_KHR) return false;
    retire_swapchain();
    if (dev.surface) vkDestroySurfaceKHR(dev.instance, dev.surface, nullptr);
    dev.surface = VK_NULL_HANDLE;
    dev.present_queue_supported = false;
    static unsigned logged = 0;
    if (logged++ < 8) logf("vk: presentation surface lost; will retry with a replacement");
    return true;
}

namespace {
bool open_window_surface(bool log_failure);
void window_size(uint32_t* width, uint32_t* height);
}

bool create_swapchain(uint32_t width, uint32_t height) {
    present_thread_drain();   // (no present of the old swapchain still being made on the presenter thread)
    if (!dev.surface) {
        window_size(&width, &height);
        if (!width || !height) return false;
        using Clock = std::chrono::steady_clock;
        static Clock::time_point next_attempt;
        const auto now = Clock::now();
        if (now < next_attempt) return false;
        next_attempt = now + std::chrono::milliseconds(250);
        if (!open_window_surface(false)) return false;
    }
    VkSurfaceCapabilitiesKHR caps;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical, dev.surface, &caps);
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS) {
        logf("vk: querying surface capabilities failed (VkResult %d)", (int)result);
        return false;
    }
    if (caps.currentExtent.width != UINT32_MAX) width = caps.currentExtent.width, height = caps.currentExtent.height;
    logf("vk: surface caps images %u..%u extent %ux%u..%ux%u usage %#x alpha %#x transform %#x",
         caps.minImageCount, caps.maxImageCount, caps.minImageExtent.width, caps.minImageExtent.height,
         caps.maxImageExtent.width, caps.maxImageExtent.height, caps.supportedUsageFlags,
         caps.supportedCompositeAlpha, caps.supportedTransforms);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        logf("vk: surface does not support transfer-destination swapchain images");
        return false;
    }
    if (!width || !height) return false;
    uint32_t n = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, nullptr);
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS || !n) {
        logf("vk: querying surface formats failed (VkResult %d, count %u)", (int)result, n);
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(n);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, formats.data());
    if (invalidate_lost_surface(result) || result != VK_SUCCESS || !n) return false;
    formats.resize(n);
    VkSurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats)   // (an UNORM target: the game's window textures are presented as they are)
        // (in sRGB: RADV on Wayland lists every format again with PASS_THROUGH_EXT and the compositor's colour spaces,
        // and the last UNORM entry left the compositor an untagged surface, a grey picture. Still the last sRGB one, so
        // a driver that lists only sRGB (NVIDIA, X11, Windows) keeps the format it had.)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fmt = f;
    // (hookshot) the Insert menu's HDR: a 10-bit HDR10 surface when the display lists one (Windows: "Use HDR" on in its display
    // settings), the picture then written by framegen_hdr. Otherwise the SDR one above, said once
    dev.hdr = false;
    if (opts.hdr) {
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
                f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
                fmt = f, dev.hdr = true;
        static bool told;
        if (!told) {
            told = true;
            if (dev.hdr)
                logf("vk: HDR: an HDR10 surface (format %d), paper white %d nits, peak %d nits", (int)fmt.format, opts.hdr_white,
                     opts.hdr_peak);
            else
                logf("vk: HDR: the display lists no HDR10 surface%s: SDR",
                     dev.swapchain_colorspace ? " (is HDR on in the display settings?)" : " (no VK_EXT_swapchain_colorspace)");
        }
    }
    // mailbox when the driver has it (latest frame, no tear). The game paces itself (nn_vi.c).
    n = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes;
    if (result == VK_SUCCESS && n) {
        modes.resize(n);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, modes.data());
    }
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS || modes.empty())
        logf("vk: querying present modes failed (VkResult %d, count %u); trying mandatory FIFO mode", (int)result, n);
    // (hookshot) the Insert menu's V-Sync (menu_vsync: RX_VK_FIFO=1 On, RX_VK_IMMEDIATE=1 Off for a run): On is FIFO, as on a
    // card or driver with vsync forced; Auto and Off as below. FIFO is the one mode every driver has
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    const int vsync = menu_vsync();
    // (hookshot) V-Sync Auto with DLSS Frame Generation at 2x and up: FIFO. Mailbox, NVIDIA's Fast Sync, puts the made frames on
    // the refresh grid whenever they come slower than the display (DLSS Frame Gen 3x: ~135 shown on a 165 Hz screen), so evenly
    // timed presents still reached the screen unevenly; FIFO on a VRR display shows each one when it is presented. Only where
    // that was measured (swapchain_dlssg): the renderer's own frame generation and other GPUs keep Auto as it was, also when
    // DLSS-G is asked for but the renderer's own makes the frames (a multiplier over what its runtime makes, or given up)
    const bool fg_fifo = vsync == 0 && swapchain_dlssg();
    if (fg_fifo) logf("vk: V-Sync Auto with DLSS Frame Generation at %dx: FIFO", swapchain_framegen());
    else if (vsync == 0 && swapchain_framegen() >= 2 && dlss_fg_wanted() && dlss_why() == DLSS_OK)
        logf("vk: V-Sync Auto with frame generation at %dx, its frames the renderer's own (DLSS-G does not make them): Auto's "
             "mode, as without DLSS-G", opts.framegen);
    if (vsync != 1) {
        if (!fg_fifo)
            for (auto m : modes)
                if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        // (Windows, no mailbox: AMD's driver has none, and FIFO held anything under 60 fps at 30.
        //  Linux: immediate on NVIDIA's X11 path turns vblank off for every output, so the other
        //  monitor and other programs tear: Auto leaves it to V-Sync Off, which asks for it.)
        const bool want_immediate = vsync == 2;
#ifdef _WIN32
        if (want_immediate || (mode == VK_PRESENT_MODE_FIFO_KHR && !fg_fifo))
#else
        if (want_immediate)
#endif
            for (auto m : modes)
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = dev.surface;
    ci.minImageCount = std::max(caps.minImageCount, 4u);   // (hookshot: mailbox with frame interpolation: an image always free)
    // (hookshot) A fixed frame generation multiplier's set is presented ahead (vk_framegen.cpp present_ahead_wanted): each
    // of its frames holds an image from the copy to its present, so the set needs as many spare images as it has frames. 3x
    // and 4x ask for that many over the surface's least (an NVIDIA surface: 5 and 6 images); a set the chain cannot hold is
    // shown one at a time as before
    if (swapchain_framegen() >= 3) ci.minImageCount = std::max(ci.minImageCount, caps.minImageCount + (uint32_t)swapchain_framegen());
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = {width, height};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // The renderer copies into swapchain images. Readback is optional and only available on surfaces that allow it.
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // (hookshot) the overlays Dear ImGui draws (vk_imgui.cpp) render into it. Every surface allows it (the Vulkan spec)
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) {
        for (VkCompositeAlphaFlagBitsKHR alpha : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                                  VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & alpha) { ci.compositeAlpha = alpha; break; }
    }
    ci.presentMode = mode;
    dev.swap_mode = mode;
    uint32_t sharing[3] = {dev.queue_family};
    uint32_t sharing_n = 1;
    auto share = [&](uint32_t family) {
        for (uint32_t i = 0; i < sharing_n; i++)
            if (sharing[i] == family) return;
        sharing[sharing_n++] = family;
    };
    if (dev.present_queue_supported) share(dev.present_family);
    if (dev.presenter_supported) share(dev.presenter_family);   // (the presenter thread's queue)
    if (sharing_n > 1) {   // (presented from another family: no ownership moves)
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = sharing_n;
        ci.pQueueFamilyIndices = sharing;
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = dev.swapchain;
    // (hookshot) an sRGB swapchain (some drivers list no UNORM format) made mutable when the driver can: the overlays draw
    // through UNORM views, their colours as they are, as on a UNORM swapchain. Without it, vk_imgui.cpp makes them linear
    const VkFormat view_formats[2] = {fmt.format, unorm_of(fmt.format)};
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    const bool mutable_view = dev.swap_mutable && view_formats[1] != fmt.format && (ci.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (mutable_view) {
        ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        format_list.viewFormatCount = 2;
        format_list.pViewFormats = view_formats;
        format_list.pNext = ci.pNext;
        ci.pNext = &format_list;
    }
    reflex_swapchain_info(ci);   // (NVIDIA Reflex: latency mode on)
    VkSwapchainKHR sc;
    result = vkCreateSwapchainKHR(dev.device, &ci, nullptr, &sc);
    // Passing oldSwapchain retires it even when creation fails. Nothing may acquire from it again.
    retire_swapchain();
    if (invalidate_lost_surface(result)) return false;
    if (result != VK_SUCCESS) {
        logf("vk: vkCreateSwapchainKHR failed (VkResult %d, extent %ux%u, format %d, usage %#x, mode %d, images %u)",
             (int)result, width, height, (int)fmt.format, ci.imageUsage, (int)mode, ci.minImageCount);
        return false;
    }
    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, nullptr) != VK_SUCCESS || !n) {
        vkDestroySwapchainKHR(dev.device, sc, nullptr);
        return false;
    }
    std::vector<VkImage> images(n);
    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, images.data()) != VK_SUCCESS || !n) {
        vkDestroySwapchainKHR(dev.device, sc, nullptr);
        return false;
    }
    images.resize(n);
    std::vector<VkSemaphore> rendered(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& semaphore : rendered) {
        if (vkCreateSemaphore(dev.device, &si, nullptr, &semaphore) != VK_SUCCESS) {
            for (auto made : rendered)
                if (made) vkDestroySemaphore(dev.device, made, nullptr);
            vkDestroySwapchainKHR(dev.device, sc, nullptr);
            return false;
        }
    }
    dev.swapchain = sc;
    dev.swap_rendered = std::move(rendered);
    dev.swap_format = fmt.format;
    dev.swap_extent = {width, height};
    dev.swap_images = std::move(images);
    dev.swap_spare = n > caps.minImageCount ? n - caps.minImageCount : 0;
    logf("vk: swapchain %ux%u, %u images, %s (V-Sync %s)", width, height, n,
         mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo",
         vsync == 1 ? "On" : vsync == 2 ? "Off" : "Auto");
    swap_views_make(ci.imageUsage, mutable_view ? view_formats[1] : fmt.format);
    reflex_swapchain_created(sc);   // (NVIDIA Reflex: its sleep mode on the new swapchain)
    return true;
}

namespace {

HWND main_window = nullptr;
// (hookshot) hookshot --prepare-shaders (a builder, after a build): the same work with no game, its progress as
// "@@progress" lines on stdout. A skip there leaves hookshot_prepare_skipped beside the game: its own starts then
// leave the work out too (what was not prepared is built when first drawn), until a --prepare-shaders run ends it
bool prepare_only() { return env_on("HOOKSHOT_PREPARE_ONLY"); }
std::string skipped_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string path(exe, n);
    return path.substr(0, path.find_last_of("\\/") + 1) + "hookshot_prepare_skipped";
}
// (hookshot) The --prepare-shaders run's watchdog, for any GPU and any part of it.
// - What it waits on, in the log the builder shows: every 30 s that nothing has moved, the part, its count, and whether the
//   GPU driver faulted; a driver fault as soon as it is caught. (The pipeline steps log their own progress every 10 s,
//   vk_draw.cpp prepare_progress_log, #2186; this adds the shader programs' count when the log has been quiet for 30 s.)
//   Before, the builder's details could not tell a slow driver from a stuck one.
// - When it stops for good: a driver that faults inside a pipeline compile (caught by guarded_graphics: "the driver faulted
//   compiling", above) can leave that compile unfinished inside it, and builds behind it then wait for ever (on AMD, any build of
//   the same program: #1823's dump of issue 996): builds stuck on players' cards of all kinds, AMD and NVIDIA (#2107). Whatever
//   the cause, a part that needs the stuck driver cannot go on. So when nothing has moved for RX_VK_PREPARE_STALL_S seconds
//   (300; 0: never stopped. Long: some steps read the caches or
//   decide the predictions for a while with no count, on a slow disk or CPU too), the run says where it stopped and why, leaves
//   hookshot_prepare_skipped so the game's own starts do not stop in the same place (what was not prepared is built when first
//   drawn), and ends at once with exit status 4 (make_game.py: "the GPU driver stopped responding"). Not through exit(): that
//   unloads the driver, which can wait on the same lock. Before, it waited until make_game.py stopped it after 15 minutes with no
//   line, and the game's first start then stopped in the same place
// - The pipeline cache's save, the last part, has no count and can take minutes on a large cache. When the run went on from a
//   part and left workers inside the driver (prepare_left_workers), or the driver faulted, the save can wait on them for ever
//   (vkGetPipelineCacheData), and so can exit() unloading the driver: then the save is stopped too, after twice the parts' limit
//   (600 s), and a run that gets through it ends at once (prepare_end). Twice: a fault AMD's retry recovered from ("recovered
//   with optimization disabled") counts as one, so on such a card every run is risky, and a save that is only slow should not be
//   stopped at the parts' limit. Otherwise the save is only reported, and its lines stop after the parts' 300 s so that
//   make_game.py's 15 minutes with no line (PREPARE_SILENCE) still stop a save that never ends
/// (where the run is, for the watchdog: under one lock, so it reads a part with its own count)
struct PrepareSeen {
    int part = -1;
    size_t done = 0, total = 0;
    int64_t moved_ms = 0;
};
std::mutex prepare_seen_mu;
PrepareSeen prepare_seen;
/// A part's count, from its progress: the watchdog's clock starts again when the part or its count changes. Part 3: the save
void prepare_moved(int part, size_t done, size_t total) {
    std::lock_guard lk(prepare_seen_mu);
    if (part != prepare_seen.part || done != prepare_seen.done || !prepare_seen.moved_ms)
        prepare_seen = {part, done, total, steady_ms()};
}
/// The run ends now: the log written out, then TerminateProcess, not exit(), which unloads a driver that can still hold workers
/// (@@done first for a run that finished: the line the game prints after the preparation, host/hos_main.c)
[[noreturn]] void prepare_end(int code) {
    if (host && host->log_flush) host->log_flush();
    if (!code) std::printf("@@done shaders\n");
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    std::_Exit(code);
}
std::atomic<int> driver_faults_seen{0};
void prepare_watchdog() {
    static const int stall_s = getenv("RX_VK_PREPARE_STALL_S") ? atoi(getenv("RX_VK_PREPARE_STALL_S")) : 300;
    // (a status line after this long with no other line; RX_VK_PREPARE_STATUS_S=<s>, for the tests)
    static const int64_t quiet_s = getenv("RX_VK_PREPARE_STATUS_S") ? std::max(1, atoi(getenv("RX_VK_PREPARE_STATUS_S"))) : 30;
    prepare_moved(0, 0, 0);
    std::thread([] {
        static const char* const parts[] = {"the shader programs", "the predicted pipelines", "the noted pipelines",
                                            "the pipeline cache's save"};
        const int64_t limit_s = stall_s > 0 ? stall_s : 300;   // (the save's lines stop after this, whatever RX_VK_PREPARE_STALL_S)
        const int save_stall_s = stall_s * 2;   // (a risky save's stop: twice the parts', above)
        for (;;) {
            Sleep(1000);
            const int64_t now = steady_ms();
            PrepareSeen at;
            {
                std::lock_guard lk(prepare_seen_mu);
                at = prepare_seen;
            }
            const char* what = at.part >= 0 && at.part < 4 ? parts[at.part] : "the preparation";
            const int64_t still = (now - at.moved_ms) / 1000;
            const int faults = driver_faults();
            const bool left = prepare_left_workers();
            const bool risky = faults > 0 || left;   // (the driver may be stuck: the save too can wait for ever)
            // (what it waits on: after a fault in the driver, most likely the driver; else the driver or the disk, a slow one
            // reading the caches)
            const char* waiting_on = faults ? "waiting on the GPU driver, which faulted (above)"
                                     : left ? "waiting on the GPU driver, which still has pipelines it did not finish"
                                            : "waiting on the GPU driver or the disk";
            if (faults != driver_faults_seen.exchange(faults))
                logf("vk: preparation: the GPU driver faulted %d time%s so far (above): a driver can stop responding after one, so a "
                     "stop here would be why", faults, faults == 1 ? "" : "s");
            if (stall_s > 0 && (at.part < 3 ? still >= stall_s : risky && still >= save_stall_s)) {
                if (at.part < 3)
                    logf("vk: preparation: STOPPED: nothing moved for %d s in %s (%zu of %zu): the GPU driver stopped responding%s. "
                         "The rest is built in play, when first drawn; updating the GPU driver may help", stall_s, what, at.done,
                         at.total, faults ? " after it faulted (above)" : "");
                else
                    logf("vk: preparation: STOPPED: %s waited %d s on the GPU driver, %s. The pipelines are made; only this "
                         "file is left out. Updating the GPU driver may help", what, save_stall_s,
                         faults ? "which faulted (above)" : "which still has pipelines it did not finish");
                if (FILE* f = fopen(skipped_path().c_str(), "wb")) fclose(f);
                prepare_end(4);
            }
            // (its own lines, at most one every quiet_s: while nothing has moved, what it waits on, whatever else the log says (the
            // pipeline steps' own lines, vk_draw.cpp prepare_progress_log, come every 10 s and repeat their count while it is
            // stuck); while the shader programs move, their count when the log has been quiet (the pipeline steps log theirs);
            // the save, below)
            static int64_t said_ms = 0;
            if (now - said_ms < quiet_s * 1000) continue;
            const bool quiet = now - last_log_ms.load(std::memory_order_relaxed) >= quiet_s * 1000;
            if (at.part >= 3) {
                if (!quiet) continue;
                if (risky)
                    logf("vk: preparation: still writing %s (%lld s), %s. Stopped at %d s if it does not end", what,
                         (long long)still, waiting_on, save_stall_s);
                else if (still < limit_s)
                    logf("vk: preparation: still writing %s (%lld s): every pipeline is made, this is the disk", what,
                         (long long)still);
                else
                    continue;   // (silent from here: make_game.py's 15 minutes with no line stop a save that never ends)
            } else if (still >= quiet_s) {
                if (stall_s > 0)
                    logf("vk: preparation: waiting: nothing in %s has finished for %lld s (%zu of %zu), %s. Stopped at %d s if "
                         "nothing moves", what, (long long)still, at.done, at.total, waiting_on, stall_s);
                else
                    logf("vk: preparation: waiting: nothing in %s has finished for %lld s (%zu of %zu), %s", what,
                         (long long)still, at.done, at.total, waiting_on);
            } else if (at.part == 0 && quiet) {
                logf("vk: preparation: still working on %s: %zu of %zu", what, at.done, at.total);
            } else {
                continue;
            }
            said_ms = now;
            if (host && host->log_flush) host->log_flush();
        }
    }).detach();
}
// (hookshot) The first start after an update prepares every shader (prepare_run) and the pipelines the last sessions noted
// (precompile_run): a minute and more, before the game's first frame. They run on a thread of their own while this one, the
// renderer's start, shows the window at once with what they are doing on it, and keeps it shown and sized; on Windows it is also
// the window's thread, and handles its messages meanwhile (it would not answer otherwise). Closing the window stops them
// (mkwvk_close_asked): what was made is kept, the next start goes on from there. RX_VK_STARTUP_SCREEN=0: as before, all
// of it before the window shows anything (and a console with the count on Windows)
std::atomic<int> prep_stage{0};   // 0 the programs, 1 the pipelines the notes predict (a first start), 2 the noted pipelines
std::atomic<size_t> prep_done{0}, prep_total{0};
// (the parts' guesses before they run, and the parts that do not run: the one bar's total, PrepareBar)
std::atomic<size_t> prep_guess[3]{};
std::atomic<bool> prep_skip[3]{};
std::atomic<bool> prep_running{false}, prep_finished{false};
HANDLE prep_ended = nullptr;
#ifndef _WIN32
const MkwVkWindow* prep_window = nullptr;   // (its size)
#endif
void prep_progress(size_t done, size_t total) {
    prep_done.store(done, std::memory_order_relaxed);
    prep_total.store(total, std::memory_order_relaxed);
}
bool screen_wanted() {
    const char* e = getenv("RX_VK_STARTUP_SCREEN");
    return !e || atoi(e) != 0;
}
/// The window's size now (0x0: minimized, or unknown)
void window_size(uint32_t* w, uint32_t* h) {
    *w = *h = 0;
#ifdef _WIN32
    RECT r{};
    if (main_window && GetClientRect(main_window, &r)) *w = (uint32_t)(r.right - r.left), *h = (uint32_t)(r.bottom - r.top);
#else
    int width = 0, height = 0;
    if (prep_window) prep_window->drawable_size(prep_window->window, &width, &height);
    if (width > 0 && height > 0) *w = (uint32_t)width, *h = (uint32_t)height;
#endif
}
void prepare_with_screen() {
    prep_ended = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    prep_running.store(true);
    std::thread worker([] {
        const bool update = prepare_is_update();   // (an update, RX_VK_PREPARE_BACKGROUND=1: what the notes name now, the rest in play)
        {
            size_t guess[3];
            prepare_guess(guess);
            for (int k = 0; k < 3; k++) prep_guess[k].store(guess[k]);
        }
        if (update) prep_skip[0].store(true), prep_skip[1].store(true);
        const bool prepared = !update && prepare_run(prep_progress, false);
        if (!prepared) prep_skip[0].store(true);
        // (a first start, or one after a start that did not finish them: the pipelines the notes predict, into the driver's cache)
        if ((prepared || (!update && predict_pending())) && !prepare_stopped()) {
            prep_done.store(0), prep_total.store(0);
            prep_stage.store(1);
            predict_run(prep_progress);
        } else {
            prep_skip[1].store(true);
        }
        if (!prepare_stopped()) {
            prep_done.store(0), prep_total.store(0);
            prep_stage.store(2);
            precompile_run(prep_progress);
        }
        if (prepare_stopped()) pipeline_cache_save(true);   // (the driver's part built so far, for the next start)
        else if (update) prepare_background();
        prep_finished.store(true, std::memory_order_release);
        SetEvent(prep_ended);
    });
    // (the screen is drawn and presented on a thread of its own: a present can wait for as long as the window cannot be seen (FIFO
    // on Wayland, a locked screen), and that must hold neither the window's messages nor the game's start)
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    static std::atomic<bool> paint_stop{false};
    HANDLE painted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread painter([t0, painted] {
        ProgressClock eta;   // (over the whole bar: its units, PrepareBar)
        PrepareBar bar;
        double next_log = 10;
        bool prepared = false;   // (this start prepares the programs: a first start after an update, two steps)
        uint32_t last_w = dev.swap_extent.width, last_h = dev.swap_extent.height;
        for (;;) {
            const bool finished = prep_finished.load(std::memory_order_acquire);
            const double secs = std::chrono::duration<double>(clock::now() - t0).count();
            const int stage = prep_stage.load();
            const size_t done = prep_done.load(std::memory_order_relaxed), total = prep_total.load(std::memory_order_relaxed);
            for (int k = 0; k < 3; k++) {
                bar.guess(k, prep_guess[k].load(std::memory_order_relaxed));
                if (prep_skip[k].load(std::memory_order_relaxed)) bar.skip(k);
            }
            if (total) bar.at(stage, done, total);
            else bar.reach(stage);
            if (finished) bar.finish();
            const double whole = bar.fraction();
            eta.note(secs, (size_t)(bar.units_done() * 100), (size_t)(bar.units_total() * 100));
            const double left = eta.left();
            if (secs >= next_log && total) {
                logf("vk: start-up: %s %zu of %zu, %.0f%% of all%s%s", stage == 2 ? "pipelines" : stage ? "predicted pipelines" : "shaders", done,
                     total, whole * 100, left >= 0 ? ", " : "", left_text(left).c_str());
                next_log = secs + 10;
            }
            if (stage == 0 && total) prepared = true;
            StartupState st;
            st.stage = stage, st.done = done, st.total = total, st.left = left, st.secs = secs;
            st.prepared = prepared, st.finished = finished, st.stopping = prepare_stopped();
            st.whole = whole;
            const StartupView view = startup_view(st);
            startup_screen(&view);
            uint32_t w, h;
            window_size(&w, &h);
            if (w && h && (w != last_w || h != last_h)) {   // (resized: the window's thread leaves it to the renderer, not started yet)
                dev.swap_resize.store((uint64_t)w << 32 | h, std::memory_order_release);
                last_w = w, last_h = h;
            }
            if (w && h) present_image(nullptr, dev.swap_extent.width, dev.swap_extent.height, nullptr);
            if (finished || paint_stop.load()) break;
            Sleep(33);
        }
        // (the end: a fade to black over 0.4 s, so the game's first frame comes out of black, not cut in over the screen; none
        // after a screen of under 1.5 s, a blink, or when the window is not shown (paint_stop: its presents wait))
        const double shown = std::chrono::duration<double>(clock::now() - t0).count();
        if (shown >= 1.5 && !paint_stop.load()) {
            const auto f0 = clock::now();
            for (;;) {
                const double f = std::chrono::duration<double>(clock::now() - f0).count() / 0.4;
                startup_fade_to((float)std::min(f, 1.0));
                uint32_t w, h;
                window_size(&w, &h);
                if (w && h) present_image(nullptr, dev.swap_extent.width, dev.swap_extent.height, nullptr);
                if (f >= 1 || paint_stop.load()) break;
                Sleep(8);
            }
        }
        SetEvent(painted);
    });
    // (this thread: the window's messages on Windows, until the work is done)
    for (;;) {
#ifdef _WIN32
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
#endif
        if (WaitForSingleObject(prep_ended, 33) == WAIT_OBJECT_0) break;
    }
    worker.join();
    // (the painter ends by itself once the work is finished, after its fade; it is told to stop only when it has not by then)
    if (WaitForSingleObject(painted, 1500) == WAIT_OBJECT_0) painter.join();
    else {
        paint_stop.store(true);   // (its present waits for the window to be seen: the game's own presents will, too)
        logf("vk: start-up: the window is not being shown; the game starts without waiting for it");
        painter.detach();
    }
    startup_screen(nullptr);
    prep_running.store(false);
    logf("vk: start-up: the shaders took %.1f s", std::chrono::duration<double>(clock::now() - t0).count());
}
bool started_up() {
    if (const char* e = getenv("RX_VK_START_DELAY")) Sleep((DWORD)atoi(e) * 1000);   // (diagnosis: a long start, as the first one is)
    if (!replay_start()) return false;
    driver_cache_log("at the start");
    if (prepare_only()) {
        // (three parts: the programs, the pipelines the notes predict, then those they name, on one bar whose total has all three
        // from the start: PrepareBar. The fixed shares it had (70%, 20%, 10%) kept the predicted pipelines' minutes in a fifth)
        static int part;
        static PrepareBar bar;
        prepare_watchdog();
        const auto progress = [](size_t done, size_t total) {
            static int last = -1;
            if (!total) return;
            prepare_moved(part, done, total);
            bar.at(part, done, total);
            const int at = (int)(bar.fraction() * 1000);
            if (at == last) return;
            last = at;
            printf("@@progress %.3f\n", at / 1000.0);
            fflush(stdout);
        };
        printf("@@stage shaders Preparing shaders\n");
        fflush(stdout);
        {
            size_t guess[3];
            prepare_guess(guess);
            for (int k = 0; k < 3; k++) bar.guess(k, guess[k]);
        }
        part = 0;
        const bool prepared = prepare_run(progress, false, true);
        const bool predict = prepared || predict_pending();   // (or the last run did not finish the prediction)
        if (!prepared) {
            if (!bar.counted[0]) bar.skip(0);   // (an already prepared seed can still import pipeline states)
            if (!predict) bar.skip(1);
        }
        part = 1;
        prepare_moved(1, 0, 0);
        if (predict) predict_run(progress);
        part = 2;
        prepare_moved(2, 0, 0);
        bar.reach(2);
        precompile_run(progress);
        prepare_moved(3, 0, 0);   // (the pipeline cache written out: the watchdog reports it, and stops it only when risky)
        pipeline_cache_save(true);
        remove(skipped_path().c_str());
        if (prepare_left_workers() || driver_faults() > 0) {   // (exit() would unload a driver that may still hold workers)
            logf("vk: preparation: done; the run ends at once (%s)",
                 prepare_left_workers() ? "pipelines left in the driver" : "the driver faulted");
            prepare_end(0);
        }
        return true;
    }
    if (FILE* f = fopen(skipped_path().c_str(), "rb")) {
        fclose(f);
        logf("vk: the shaders' preparation was skipped at the build: left to when each is first drawn");
        return true;
    }
    if (precompile_wanted()) {   // (a release build, before the game: nothing to build during play)
        if (!dev.offscreen && screen_wanted()) {
            prepare_with_screen();
            return true;
        }
        // (a console while it lasts: there is no window yet, and a long wait with nothing on screen looks like a hang)
        static HANDLE console = nullptr;
        const auto progress = [](size_t done, size_t total) {
            static bool tried = false;
            if (!total) return;
            if (!tried) {
                tried = true;
                if (AllocConsole()) {
#ifdef _WIN32
                    // This console is ours. Selection must not block preparation's progress writes.
                    const HANDLE input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                     nullptr, OPEN_EXISTING, 0, nullptr);
                    if (input != INVALID_HANDLE_VALUE) {
                        DWORD mode;
                        if (GetConsoleMode(input, &mode))
                            SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
                        CloseHandle(input);
                    }
#endif
                    SetConsoleTitleA("Hookshot");
                    console = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (console == INVALID_HANDLE_VALUE) console = nullptr;
                }
            }
            {   // (in the log too, every 10 s: a run with no window has nothing else to show it)
                static DWORD next_log = 0;
                const DWORD now = GetTickCount();
                if (!next_log) next_log = now + 10000;
                if ((int32_t)(now - next_log) >= 0) {
                    logf("vk: start-up: %zu of %zu", done, total);
                    next_log = now + 10000;
                }
            }
            if (!console) return;
            char line[80];
            const int n = snprintf(line, sizeof line, "%cPreparing shaders: %zu of %zu (%zu%%)   ", 13, done, total, done * 100 / total);
            DWORD wrote = 0;
            WriteFile(console, line, (DWORD)n, &wrote, nullptr);
        };
        const bool update = prepare_is_update();   // (an update: what the notes name now, the rest in play)
        if (!update && (prepare_run(progress, false) || predict_pending())) predict_run(progress);
        if (update) state_list_run();
        precompile_run(progress);
        if (update) prepare_background();
        if (console) CloseHandle(console), console = nullptr, FreeConsole();
    }
    return true;
}
// (the window's surface made: the presents' own queue kept where its family can present to it, create_device)
static void choose_present_queue() {
    dev.presenter_supported = false;
    if (dev.presenter_queue) {
        VkBool32 ok = VK_FALSE;
        dev.presenter_supported = vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.presenter_family, dev.surface, &ok) == VK_SUCCESS && ok;
        if (!dev.presenter_supported) logf("vk: the presenter thread's queue family %u cannot present to the window: presents on the game's thread", dev.presenter_family);
    }
    dev.present_queue_supported = false;
    if (!dev.present_queue) return;
    VkBool32 ok = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.present_family, dev.surface, &ok) != VK_SUCCESS || !ok) {
        logf("vk: queue family %u cannot present to the window: presents on the replay's queue", dev.present_family);
        return;
    }
    dev.present_queue_supported = true;
    logf("vk: presents on a queue of their own, of compute family %u (RX_VK_PRESENT_QUEUE=0: off)", dev.present_family);
}

bool open_window_surface(bool log_failure) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleA(nullptr);
    si.hwnd = main_window;
    if (!IsWindow(main_window) || vkCreateWin32SurfaceKHR(dev.instance, &si, nullptr, &surface) != VK_SUCCESS) return false;
#else
    const MkwVkWindow* w = prep_window;
    if (!w || !w->create_surface(w->window, (void*)dev.instance, (void*)&surface)) {
        if (log_failure)
            logf("vk: SDL_Vulkan_CreateSurface failed; check SDL_VIDEODRIVER and the Vulkan window-system extensions");
        return false;
    }
#endif
    VkBool32 ok = VK_FALSE;
    const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queue_family, surface, &ok);
    if (result != VK_SUCCESS || !ok) {
        if (log_failure) logf("vk: queue-family present support failed (VkResult %d, supported %u)", (int)result, ok);
        vkDestroySurfaceKHR(dev.instance, surface, nullptr);
        return false;
    }
    dev.surface = surface;
    choose_present_queue();
    return true;
}

/// (hookshot) What this PC reports about itself, for the Quality preset Auto (this PC) (auto_settings.h, vk_menu.cpp
/// menu_auto_for_gpu): once, when the window is known (its screen's size and refresh), the card already chosen
void auto_profile() {
    static bool done;
    if (done) return;
    done = true;
    ::auto_settings::Hardware hw;
    const VkPhysicalDeviceProperties& p = dev.props;
    hw.vendor = p.vendorID == 0x10de ? ::auto_settings::NVIDIA : p.vendorID == 0x1002 ? ::auto_settings::AMD
              : p.vendorID == 0x8086 ? ::auto_settings::INTEL : ::auto_settings::OTHER;
    hw.integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    for (uint32_t i = 0; i < dev.memory.memoryHeapCount; i++)
        if (dev.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            hw.vram_mb = std::max<uint32_t>(hw.vram_mb, (uint32_t)(dev.memory.memoryHeaps[i].size >> 20));
    hw.mesh_native = dev.mesh_capable;   // (the card's, not this run's mode: Linux AMD's Automatic is Compute, #2245)
    hw.mesh_as_compute = dev.mesh_capable && !dev.mesh_shader;
    hw.rtx = hw.vendor == ::auto_settings::NVIDIA && strstr(p.deviceName, "RTX");
    // (RDNA 4: "AMD Radeon RX 9070 XT", RADV's "(RADV GFX1201)")
    hw.rdna4 = hw.vendor == ::auto_settings::AMD && (strstr(p.deviceName, "RX 9") || strstr(p.deviceName, "GFX12"));
    // (the Steam Deck's APU, LCD and OLED, or SteamOS's own flag)
    const char* deck = getenv("SteamDeck");
    hw.handheld = (deck && atoi(deck) == 1) || strstr(p.deviceName, "Custom GPU 0405") || strstr(p.deviceName, "Custom GPU 0932");
    hw.dlss_built = dlss_built();
    hw.fsr4_built = fsr4_built();
    hw.threads = (int)std::thread::hardware_concurrency();
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) hw.ram_mb = (uint32_t)(m.ullTotalPhys >> 20);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (main_window && GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hw.screen_w = (int)dm.dmPelsWidth, hw.screen_h = (int)dm.dmPelsHeight;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) hw.ram_mb = (uint32_t)(((uint64_t)pages * (uint64_t)page) >> 20);
    const MkwVkWindow* w = (const MkwVkWindow*)main_window;
    if (w && w->version >= 3 && w->desktop_size) w->desktop_size(w->window, &hw.screen_w, &hw.screen_h);
#endif
    hw.refresh_hz = (display_refresh_mhz() + 500) / 1000;
    menu_auto_for_gpu(p, hw);
}

int start(void* hwnd) {
    main_window = (HWND)hwnd;
    auto_profile();   // (the Quality preset Auto (this PC): before the game first asks its mode, which reads the options)
    if (!hwnd) {   // (HOS_HEADLESS: no window; the frames are rendered all the same, RX_VK_SHOTS writes them out)
        dev.offscreen = true;
        dev.swap_extent = {1920, 1080};
        dev.swap_format = VK_FORMAT_R8G8B8A8_UNORM;
        logf("vk: no window: presents render offscreen (%ux%u)", dev.swap_extent.width, dev.swap_extent.height);
        if (!started_up()) return 0;
        pipeline_cache_play();   // (play begins: what is made from now on goes into the play files)
        return 1;
    }
#ifdef _WIN32
    if (!open_window_surface(true)) return 0;
    RECT r{};
    GetClientRect((HWND)hwnd, &r);
    if (!create_swapchain((uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top))) return 0;
#else
    const MkwVkWindow* w = (const MkwVkWindow*)hwnd;
    prep_window = w;
    if (w->version < 1 || w->version > MKWVK_WINDOW_VERSION) {   // (1: no refresh_mhz)
        logf("vk: unsupported SDL window bridge version %u (expected %u)", w->version, MKWVK_WINDOW_VERSION);
        return 0;
    }
    if (!open_window_surface(true)) return 0;
    int width = 0, height = 0;
    w->drawable_size(w->window, &width, &height);
    if (width <= 0 || height <= 0) {
        logf("vk: SDL returned zero drawable size %dx%d", width, height);
        return 0;
    }
    if (!create_swapchain((uint32_t)width, (uint32_t)height)) {
        logf("vk: swapchain creation failed for drawable %dx%d", width, height);
        return 0;
    }
#endif
    if (!started_up()) {
        logf("vk: renderer startup failed after window and swapchain initialization");
        return 0;
    }
    pipeline_cache_play();   // (play begins: what is made from now on goes into the play files)
    return 1;
}

// (the window's thread: only noted. The swapchain is remade by whoever presents next, in present_image: made here, it went from under a
// frame between its acquire and its submit, and the frame's commands named images of the swapchain just destroyed: a lost device)
void resize(int width, int height) {
    if (dev.offscreen || width <= 0 || height <= 0) return;
    dev.swap_resize.store((uint64_t)(uint32_t)width << 32 | (uint32_t)height, std::memory_order_release);
}

const char* describe() {
    static std::string d;
    d = std::string(dev.props.deviceName) + " (Vulkan, native)";
    return d.c_str();
}

MkwVkRenderer table;

} // namespace
} // namespace mkwvk

// (hookshot) The window is being closed (host/display.c, host/posix/display_sdl.c, before they end the process): while the
// shaders are prepared, their workers are stopped and what they made is kept first (10 s at most: a driver can take longer on one
// pipeline). Looked up by name, so an executable and a back end of different versions still close
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_close_asked(void) {
    using namespace mkwvk;
    programs_census_log("close");
    module_ids_flush();
    fsr4_close();             // (AMD FSR 4's tuned layers: their background builds stopped first, what they made then saved below)
    pipeline_cache_close();   // (in play: the pipelines made since the last save, kept for the next start)
    if (prep_running.load() && !prepare_stopped()) {
        logf("vk: the window was closed while the shaders were prepared: stopping, what is done is kept");
        prepare_stop();
        if (WaitForSingleObject(prep_ended, 10000) != WAIT_OBJECT_0) logf("vk: the preparation did not stop within 10 s: closing anyway");
    }
    dlss_fg_close();   // (NVIDIA DLSS Frame Generation through NGX: its feature let go before the process exits, vk_dlss.cpp)
}

extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const MkwVkRenderer* mkwvk_init(const MkwVkHost* h) {
    using namespace mkwvk;
    if (!h) return nullptr;
    if (h->version != MKWVK_API_VERSION) {
        std::fprintf(stderr, "vk: host version mismatch: host has %u, expected %u\n", h->version, MKWVK_API_VERSION);
        return nullptr;
    }
    host = h;
    if (!load_global()) {
        logf("vk: load_global failed (Vulkan loader library or vkCreateInstance missing)");
        return nullptr;
    }
    if (!create_instance()) {
        logf("vk: create_instance failed");
        return nullptr;
    }
    if (!pick_device()) {
        logf("vk: pick_device failed");
        return nullptr;
    }
    if (!create_device()) {
        logf("vk: create_device failed");
        return nullptr;
    }
    logf("vk: %s, Vulkan %u.%u, driver %#x; mesh shaders %s, host memory import %s, pipeline libraries %s",
         dev.props.deviceName, VK_VERSION_MAJOR(dev.props.apiVersion), VK_VERSION_MINOR(dev.props.apiVersion),
         dev.props.driverVersion, dev.mesh_shader ? "yes" : "no", dev.external_memory_host ? "yes" : "no",
         dev.graphics_pipeline_library ? "yes" : "no");
    logf("vk: driver: %s", driver_text().c_str());   // (the builders' Save my pipeline list reads it: nx2/pipeline_share.py)
    if (!dev.mesh_shader && !dev.mesh_fallback && !env_on("RX_VK_NO_MESH_WARNING")) {
        // (the game draws its foliage, grass and many effects with mesh shaders: without them those draws are left out and the
        // picture is broken (an RX 580's "static"-like scenes, 2026-11-08). Said once, on its own thread, and the game goes on.)
        // (a GPU with no buffer device addresses has no fallback to run either)
        logf("vk: this GPU has no mesh shaders: the game's foliage, grass and many effects cannot be drawn");
#ifdef _WIN32
        std::thread([name = std::string(dev.props.deviceName)] {
            const std::string text = "Your graphics card (" + name + ") or its driver has no mesh shader support, which Ocarina of Time "
                "uses for foliage, grass and many effects, so parts of the picture will be missing or broken.\n\nHookshot needs a GPU "
                "with mesh shaders: NVIDIA GeForce RTX 20 series or newer, AMD Radeon RX 6000 series or newer, or Intel Arc. If your "
                "card is one of those, update its driver.";
            MessageBoxA(nullptr, text.c_str(), "Hookshot: graphics card", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }).detach();
#endif
    }
    table.version = MKWVK_API_VERSION;
    table.start = start;
    table.resize = resize;
    table.describe = describe;
    fill_replay_table(table);
    return &table;
}

namespace mkwvk {
HWND app_window() { return main_window; }   // (hookshot: the Insert menu's Fullscreen row)

/// (hookshot) The refresh rate of the display the window is on, in mHz; 0 when not known (frame generation's Auto). Asked
/// again once a second at most: the window may move to another display, and Windows' answer costs a few system calls
int display_refresh_mhz() {
    static std::atomic<int> cached{0};
    static std::atomic<int64_t> asked_at{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last = asked_at.load(std::memory_order_relaxed);
    if (last && now - last < 1000) return cached.load(std::memory_order_relaxed);
    asked_at.store(now, std::memory_order_relaxed);
    int mhz = 0;
#ifdef _WIN32
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (main_window && GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)   // (0 and 1: the default)
        // (whole Hz: a 59.94 Hz mode often says 59, and Auto would then make a frame too few a second. One under a
        // multiple of 6 (24, 30, 60, 120, 144, 240 Hz) is taken as that rate's /1.001)
        mhz = (dm.dmDisplayFrequency + 1) % 6 == 0 ? (int)((dm.dmDisplayFrequency + 1) * 1000000ull / 1001)
                                                    : (int)dm.dmDisplayFrequency * 1000;
#else
    const MkwVkWindow* w = (const MkwVkWindow*)main_window;
    if (w && w->version >= 2 && w->refresh_mhz) mhz = w->refresh_mhz(w->window);
#endif
    cached.store(mhz, std::memory_order_relaxed);
    return mhz;
}
} // namespace mkwvk
