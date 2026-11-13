// (hookshot) AMD Anti-Lag 2 through VK_AMD_anti_lag, without AMD's SDK (vk_antilag.h).
//
// While the Insert menu's AMD Anti-Lag 2 row is on (RX_VK_ANTILAG=0 / 1 over it), each frame tells the driver two points of
// its life with vkAntiLagUpdateAMD, both with the frame's index: its input stage after the previous frame's present (where
// NVIDIA Reflex sleeps, vk_reflex.cpp: the driver waits there until the frame should start), and its present stage before
// vkQueuePresentKHR. Off is the game as without it: the driver is told Off once, nothing else is called. The extension is
// enabled whenever the device has it, so the row needs no restart: enabled alone it does nothing.
//
// With frame generation on, the frames are shown by its display thread (vk_framegen.cpp display_main), a set of made frames
// and the game's for each game frame, later than the game thread's next input stage. As NVIDIA Reflex's markers there
// (#2118), each set keeps the game frame it was made at (antilag_frame), and only the set's game frame gives that frame's
// present stage (antilag_present_frame); the made frames' presents give none.
//
// As with Reflex, the input stage stands for the game's simulation start only roughly: OoT simulates on its main thread and
// renders on its presentation thread, and the wait holds the presentation thread, which the main thread waits for.
#include "vk_antilag.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "vk_menu.h"

namespace mkwvk {

namespace {

struct AntiLag {
    bool have_ext = false, ready = false;
    VkPhysicalDeviceAntiLagFeaturesAMD feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
    PFN_vkAntiLagUpdateAMD update = nullptr;
    bool active = false;         // (the driver was last told On)
    bool frame_marked = false;   // (this frame's input stage given: its present stage counts)
    uint64_t frame = 0;          // (the frame's index, the same for both its stages)
    uint64_t since = 0;          // (the last index before the row last changed: frames up to it get no present stage)
    std::mutex mu;
};
AntiLag& al() {
    static AntiLag a;
    return a;
}

bool wanted() {
    static const char* e = getenv("RX_VK_ANTILAG");
    return (e && *e ? atoi(e) : opts.antilag) > 0;
}

/// The mode to the driver, with one of the frame's stages or none (the mode alone)
void update(bool on, const VkAntiLagPresentationInfoAMD* stage) {
    VkAntiLagDataAMD d{VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD};
    d.mode = on ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD;
    d.maxFPS = 0;   // (no limit of its own: the game's frame rate cap paces)
    d.pPresentationInfo = stage;
    al().update(dev.device, &d);
}

/// With a.mu held: the row on, and the extension and its feature. Turning off tells the driver once
bool on() {
    AntiLag& a = al();
    if (!a.ready) return false;
    const bool want = wanted();
    if (want != a.active) {
        if (!want) update(false, nullptr);
        a.active = want, a.frame_marked = false, a.since = a.frame;
        logf("vk: AMD Anti-Lag 2: %s", want ? "on" : "off");
    }
    return want;
}

void stage(VkAntiLagStageAMD s, uint64_t frame) {
    VkAntiLagPresentationInfoAMD p{VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD};
    p.stage = s, p.frameIndex = frame;
    update(true, &p);
}

}  // namespace

void antilag_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    static const char* e = getenv("RX_VK_ANTILAG");
    if (e && !strcmp(e, "-")) return;   // (RX_VK_ANTILAG=-: the extension not even enabled)
    bool have = false;
    for (const auto& x : all) have |= !strcmp(x.extensionName, VK_AMD_ANTI_LAG_EXTENSION_NAME);
    if (!have) return;
    bool listed = false;
    for (const char* w : want) listed |= !strcmp(w, VK_AMD_ANTI_LAG_EXTENSION_NAME);
    if (!listed) want.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
    al().have_ext = true;
}

void antilag_device_features(void**& tail) {
    AntiLag& a = al();
    if (!a.have_ext) return;
    *tail = &a.feature, tail = &a.feature.pNext;
}

void antilag_device_created() {
    AntiLag& a = al();
    if (!a.have_ext) return;
    if (!a.feature.antiLag) {
        logf("vk: AMD Anti-Lag 2: the device does not support it: off");
        return;
    }
    a.update = (PFN_vkAntiLagUpdateAMD)vkGetDeviceProcAddr(dev.device, "vkAntiLagUpdateAMD");
    a.ready = a.update != nullptr;
    logf("vk: AMD Anti-Lag 2: %s", a.ready ? "available (VK_AMD_anti_lag)" : "its function is missing: off");
}

bool antilag_available() { return al().ready; }

thread_local uint64_t antilag_present_frame = kAntiLagThisFrame;

uint64_t antilag_frame() {
    AntiLag& a = al();
    std::lock_guard lk(a.mu);
    return on() && a.frame_marked ? a.frame : 0;
}

void antilag_present() {
    AntiLag& a = al();
    std::lock_guard lk(a.mu);
    if (!on()) return;
    const uint64_t f = antilag_present_frame == kAntiLagThisFrame ? (a.frame_marked ? a.frame : 0) : antilag_present_frame;
    // (none: a made frame, or no input stage yet; a frame from before the row was last turned on had its input stage in
    // the driver's earlier run)
    if (!f || f <= a.since) return;
    stage(VK_ANTI_LAG_STAGE_PRESENT_AMD, f);
}

void antilag_input() {
    AntiLag& a = al();
    std::lock_guard lk(a.mu);
    if (!on()) return;
    a.frame++;
    stage(VK_ANTI_LAG_STAGE_INPUT_AMD, a.frame);   // (the driver's wait, when it wants one)
    a.frame_marked = true;
}

} // namespace mkwvk
