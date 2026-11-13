#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>
#include "vk_present_rect.h"
#include "vk_present_trace.h"
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkImage = uint64_t;
using VkImageView = uint64_t;
constexpr VkImage VK_NULL_HANDLE = 0;
constexpr int VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 1, VK_IMAGE_ASPECT_COLOR_BIT = 1;
struct VkClearColorValue { float r, g, b, a; };
struct VkImageSubresourceRange { int aspect, mip, levels, layer, layers; };
struct { struct { uint32_t width = 1720, height = 720; } swap_extent; bool hdr = false; } dev;
std::atomic<bool> present_native_pq{false};
struct Host { float (*aspect_ratio)(); };
float ratio = 43.0f / 18.0f;
int aspect_calls = 0;
float aspect_callback() { ++aspect_calls; return ratio; }
Host h{aspect_callback}; Host* host = &h;
struct { int upscale = 0, sharpness = 0, framegen = 0, hdr_contrast = 100; } opts;
std::atomic<uint32_t> present_kind{0};
bool scaled = false;
bool encoded = false;
int hdr_calls = 0;
int blits = 0, clears = 0;
PresentRect drawn{};
std::vector<std::string> messages;
void logf(const char* format, ...) {
    char b[1024]; va_list a; va_start(a, format); std::vsnprintf(b, sizeof b, format, a); va_end(a); messages.emplace_back(b);
}
void vkCmdClearColorImage(int, VkImage, int, const VkClearColorValue*, int, const VkImageSubresourceRange*) { ++clears; }
void image_barrier(int, VkImage, int, int) {}
void full_barrier(int) {}
VkImageView framegen_scaled_view() { return 19; }
VkImage framegen_hdr(int, VkImageView, uint32_t, uint32_t, const int*, uint32_t, uint32_t, bool) {
    ++hdr_calls; return encoded ? 29 : VK_NULL_HANDLE;
}
VkImage framegen_scale(int, VkImageView, VkImage, uint32_t, uint32_t, const int*, uint32_t, uint32_t, bool) { return scaled ? 9 : 0; }
void blit_crop_to_rect(int, VkImage, uint32_t, uint32_t, const int*, VkImage, int, PresentRect rect) { ++blits; drawn = rect; }
void present(VkImage src = 1, uint32_t sw = 3440, uint32_t sh = 1440, const int* crop = nullptr, VkImageView view = 2) {
    struct { int cmd = 1; } f; VkImage dst = 3;
    /* ACTUAL_DRAW */
    /* ACTUAL_TRACE */
}
void unit_state() {
    CHECK(PresentGeometryTrace::kLimit == 256);
    PresentGeometryTrace t; PresentGeometrySample s;
    CHECK(t.note(s) == PresentTraceDecision::Emit);
    CHECK(t.note(s) == PresentTraceDecision::Quiet);
    s.kind = 1; CHECK(t.note(s) == PresentTraceDecision::Emit);
    for (int i = 0; i < 10000; ++i) { s.kind = uint32_t(i % 2); CHECK(t.note(s) == PresentTraceDecision::Quiet); }
    s.kind = 2; CHECK(t.note(s) == PresentTraceDecision::Emit);
    s.host_aspect(true, std::numeric_limits<double>::quiet_NaN()); CHECK(t.note(s) == PresentTraceDecision::Emit);
    s.host_aspect(true, std::numeric_limits<double>::infinity()); CHECK(t.note(s) == PresentTraceDecision::Quiet);
    s.host_aspect(true, -1); CHECK(t.note(s) == PresentTraceDecision::Quiet);
    s.host_aspect(false, 0); CHECK(t.note(s) == PresentTraceDecision::Emit);
    s.host_aspect(true, 43.0/18.0); CHECK(t.note(s) == PresentTraceDecision::Emit);
    for (int i = 0; i < 10000; ++i) {
        s.filter = i % 3; s.sharpness = i % 101; s.framegen = i % 6;
        CHECK(t.note(s) == PresentTraceDecision::Quiet);
    }
    ++s.output[0]; CHECK(t.note(s) == PresentTraceDecision::Emit);
    PresentGeometryTrace bounded;
    for (uint32_t i = 0; i < PresentGeometryTrace::kLimit; ++i) { s.output[0] = i; CHECK(bounded.note(s) == PresentTraceDecision::Emit); }
    ++s.output[0]; CHECK(bounded.note(s) == PresentTraceDecision::Limit);
    for (int i = 0; i < 10000; ++i) { ++s.output[0]; CHECK(bounded.note(s) == PresentTraceDecision::Quiet); }
}
int main(int argc, char** argv) {
    CHECK(argc == 2); const bool enabled = std::atoi(argv[1]) != 0; unit_state();
    present(); CHECK(blits == 1 && drawn.w == 1720 && drawn.h == 720);
    CHECK(aspect_calls == 1); // tracing reuses the actual fit input, including when the callback could change.
    if (enabled) {
        CHECK(messages.size() == 1);
        CHECK(messages.back().find("kind=game source=3440x1440") != std::string::npos);
        CHECK(messages.back().find("output=1720x720 destination=0,0 1720x720") != std::string::npos);
        CHECK(messages.back().find("route=direct") != std::string::npos);
    }
    for (int i = 0; i < 100; ++i) present();
    CHECK(messages.size() == size_t(enabled));
    dev.swap_extent = {3440, 1369}; present();
    if (enabled) CHECK(messages.size() == 2 && messages.back().find("output=3440x1369") != std::string::npos);
    const int crop[] = {16, 8, 2560, 1440}; present(1, 4096, 2160, crop);
    if (enabled) CHECK(messages.back().find("crop=supplied:16,8 2560x1440 content=2560x1440") != std::string::npos);
    const size_t before_requests = messages.size();
    opts.upscale = 2; opts.sharpness = 50; present(1, 4096, 2160, crop);
    CHECK(messages.size() == before_requests);
    scaled = true; present(1, 4096, 2160, crop);
    if (enabled) CHECK(messages.back().find("route=scaled view=1 filter-request=2 sharpness-request=50") != std::string::npos);
    dev.hdr = encoded = true; present(1, 4096, 2160, crop);
    if (enabled) CHECK(messages.back().find("route=hdr") != std::string::npos);
    encoded = false; present(1, 4096, 2160, crop);
    if (enabled) CHECK(messages.back().find("route=scaled") != std::string::npos);
    encoded = true; present_native_pq = true;
    const int before_native_pq = hdr_calls;
    present(1, 4096, 2160, crop); CHECK(hdr_calls == before_native_pq + 1);   // (Native: the HDR pass, at 100% too)
    if (enabled) CHECK(messages.back().find("route=hdr") != std::string::npos);
    opts.hdr_contrast = 150; present(1, 4096, 2160, crop); CHECK(hdr_calls == before_native_pq + 2);   // (graded)
    opts.hdr_contrast = 100;
    dev.hdr = false; present_native_pq = false; encoded = false;
    present(1, 4096, 2160, crop);   // (SDR again: the game's route back to scaled)
    if (enabled) CHECK(messages.back().find("route=scaled") != std::string::npos);
    opts.framegen = 2; present_kind = 1; present(1, 2560, 1440);
    if (enabled) CHECK(messages.back().find("kind=made") != std::string::npos && messages.back().find("framegen-request=2") != std::string::npos);
    const size_t n = messages.size();
    present_kind = 0; present(1, 4096, 2160, crop);
    present_kind = 1; present(1, 2560, 1440); CHECK(messages.size() == n);
    // The game stream's request-only change and alternating frames add nothing.
    for (int i = 0; i < 100; ++i) { present_kind = 0; present(1, 4096, 2160, crop); present_kind = 1; present(1, 2560, 1440); }
    CHECK(messages.size() == n);
    scaled = false;
    const int before_startup = aspect_calls;
    present(0, 0, 0, nullptr, 0);
    CHECK(aspect_calls == before_startup);
    if (enabled) CHECK(messages.back().find("kind=startup") != std::string::npos && messages.back().find("route=clear") != std::string::npos);
    ratio = std::numeric_limits<float>::quiet_NaN(); present_kind = 0; present();
    if (enabled) CHECK(messages.back().find("host-aspect=invalid:0") != std::string::npos);
    const size_t invalid = messages.size(); present(); CHECK(messages.size() == invalid);
    host = nullptr; present();
    if (enabled) CHECK(messages.back().find("host-aspect=unavailable:0") != std::string::npos);
    const size_t before_slider = messages.size();
    for (int i = 0; i < 1000; ++i) { opts.sharpness = i % 101; present(); }
    CHECK(messages.size() == before_slider);
    ++dev.swap_extent.width; present();
    CHECK(messages.size() == before_slider + size_t(enabled));
    for (uint32_t i = 0; i < 300; ++i) { dev.swap_extent.width = 1720 + i; present(); }
    if (enabled) {
        CHECK(messages.size() == 257);
        CHECK(messages.back().find("limit of 256 changes reached") != std::string::npos);
    }
    if (!enabled) CHECK(messages.empty());
    std::puts("PASS: effective geometry/route changes, independent frame kinds, invalid-aspect stability, opt-in and bounded output");
}
