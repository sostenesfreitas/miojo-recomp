"""Execute the production present geometry/blit selection with command-recording stubs.

No GPU or game data. The rectangle helper is included directly; the consumer block
is extracted from present_image so losing the host aspect or scaling to the whole
window fails even when the helper's own tests still pass.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_replay.cpp")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    start = source.index("    const PresentAspect aspect{", source.index("void present_image("))
    end = source.index("    if (src && framelog_on())", start)
    block = source[start:end]
    header = (ROOT / "gpu/vk/vk_present_rect.h").as_posix()
    fixture = '#include "' + header + '"\n' + r'''
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkImage = uint64_t;
using VkImageView = uint64_t;
constexpr VkImage VK_NULL_HANDLE = 0;
constexpr int VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 1;
constexpr int VK_IMAGE_ASPECT_COLOR_BIT = 1;
struct VkClearColorValue { float r, g, b, a; };
struct VkImageSubresourceRange { int aspect, mip, levels, layer, layers; };
struct { struct { uint32_t width, height; } swap_extent; bool hdr = false; } dev;
std::atomic<bool> present_native_pq{false};
struct { int hdr_contrast = 100; } opts;
struct Host { float (*aspect_ratio)(); };
Host* host;
float logical_aspect;
float aspect_callback() { return logical_aspect; }
bool filtered;
int clears, blits, scales, barriers, hdr_calls, hdr_barriers, logs;
void logf(const char*, ...) { ++logs; }
bool hdr_ready = true;
uint32_t hdr_w, hdr_h;
uint32_t scale_w, scale_h;
PresentRect drawn;
const int* drawn_crop;
VkImage drawn_source;
void vkCmdClearColorImage(int, VkImage, int, const VkClearColorValue* color, int count,
                         const VkImageSubresourceRange* range) {
    CHECK(color->r == 0 && color->g == 0 && color->b == 0);
    CHECK(count == 1 && range->levels == 1 && range->layers == 1);
    ++clears;
}
void image_barrier(int, VkImage, int, int) { ++barriers; }
void full_barrier(int) { ++hdr_barriers; }
VkImageView framegen_scaled_view() { return 117; }
VkImage framegen_hdr(int, VkImageView view, uint32_t, uint32_t, const int* crop, uint32_t w, uint32_t h, bool pq) {
    CHECK(view == (filtered ? 117u : 7u) && pq == present_native_pq.load());
    if (filtered) CHECK(crop == nullptr);
    ++hdr_calls; hdr_w = w; hdr_h = h;
    return hdr_ready ? 101 : VK_NULL_HANDLE;
}
VkImage framegen_scale(int, VkImageView, VkImage, uint32_t, uint32_t, const int*, uint32_t w, uint32_t h, bool pq) {
    CHECK(pq == present_native_pq.load());   // (Native: scaled in 10 bits)
    ++scales; scale_w = w; scale_h = h;
    return filtered ? 99 : VK_NULL_HANDLE;
}
void blit_crop_to_rect(int, VkImage src, uint32_t, uint32_t, const int* crop, VkImage, int, PresentRect rect) {
    ++blits; drawn = rect; drawn_crop = crop; drawn_source = src;
}
void actual_present(VkImage src, uint32_t sw, uint32_t sh, const int* crop, VkImageView view) {
    struct { int cmd; } f{1};
    VkImage dst = 2;
''' + block + r'''
}
void expect(VkImage src, uint32_t sw, uint32_t sh, const int* crop, uint32_t dw, uint32_t dh,
            float aspect, PresentRect want, bool filter, bool with_host = true, bool with_callback = true) {
    Host h{with_callback ? aspect_callback : nullptr}; host = with_host ? &h : nullptr;
    logical_aspect = aspect; filtered = filter;
    dev.swap_extent = {dw, dh};
    clears = blits = scales = barriers = hdr_calls = hdr_barriers = 0;
    actual_present(src, sw, sh, crop, 7);
    const bool hdr_attempted = dev.hdr;   // (Native too: read through its view by the HDR pass, never blitted)
    const bool hdr_emitted = hdr_attempted && hdr_ready;
    // (Native without the HDR pass: its scaled picture (10 bits), else black; never its image blitted in the image's format)
    const bool native_black = present_native_pq.load() && !hdr_emitted && !filter;
    CHECK(blits == int(!native_black) && scales == 1);
    if (!native_black) CHECK(drawn.x == want.x && drawn.y == want.y && drawn.w == want.w && drawn.h == want.h);
    CHECK(scale_w == want.w && scale_h == want.h);
    const bool bars = want.x || want.y || want.w != dw || want.h != dh;
    CHECK(clears == int(bars) + int(native_black) && barriers == int(bars));
    CHECK(hdr_calls == int(hdr_attempted) && hdr_barriers == int(hdr_emitted));
    if (hdr_attempted) CHECK(hdr_w == want.w && hdr_h == want.h);
    if (!native_black) CHECK(drawn_source == (hdr_emitted ? 101 : filter ? 99 : src));
    if (!native_black) CHECK(drawn_crop == (hdr_emitted || filter ? nullptr : crop));
}
int main() {
    const float wide = 43.0f / 18.0f;
    const int crop[4] = {16, 8, 2560, 1440};
    const bool filters[] = {false, true};
    for (bool filter : filters) {
        expect(3, 1920, 1080, nullptr, 3440, 1440, wide, {0, 0, 3440, 1440}, filter);
        expect(3, 1920, 1080, nullptr, 1920, 1009, wide, {0, 102, 1920, 804}, filter);
        expect(3, 4096, 2160, crop, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        // The interpolation thread presents a materialized crop without the original crop pointer.
        expect(4, 2560, 1440, nullptr, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        expect(3, 1920, 1080, nullptr, 1720, 720, wide, {0, 0, 1720, 720}, filter);
        expect(3, 1920, 1080, nullptr, 1600, 1000, 0, {0, 50, 1600, 900}, filter);
        expect(3, 1920, 1080, nullptr, 1600, 1000, wide, {0, 50, 1600, 900}, filter, false);
        expect(3, 1920, 1080, nullptr, 1600, 1000, wide, {0, 50, 1600, 900}, filter, true, false);
        expect(3, 1920, 1080, nullptr, 1600, 1000, std::numeric_limits<float>::quiet_NaN(),
               {0, 50, 1600, 900}, filter);
        dev.hdr = true;
        expect(3, 4096, 2160, crop, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        hdr_ready = false; // Converter failure retains the normal source/filter and the same fitted rectangle.
        expect(3, 4096, 2160, crop, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        hdr_ready = true; present_native_pq = true; // Native PQ: the pass too, told it is PQ, at 100% contrast...
        expect(3, 4096, 2160, crop, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        opts.hdr_contrast = 150; // ... and graded.
        expect(3, 4096, 2160, crop, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        opts.hdr_contrast = 100;
        hdr_ready = false; // Native without the pass: the scaled picture, else black (said once), not the image blitted.
        expect(3, 4096, 2160, crop, 1280, 720, wide, {0, 92, 1280, 536}, filter);
        hdr_ready = true;
        present_native_pq = false; dev.hdr = false;
    }
    CHECK(logs == 1);
    // Startup has no game image: one full clear, then the window-sized overlay.
    Host h{aspect_callback}; host = &h; logical_aspect = wide;
    dev.swap_extent = {1600, 1000}; clears = blits = scales = barriers = 0;
    actual_present(0, 1600, 1000, nullptr, 0);
    CHECK(clears == 1 && blits == 0 && scales == 0 && barriers == 0);
    std::puts("PASS: host aspect, crop, normal/generated frames, filter targets, bars and restore");
}
'''
    with tempfile.TemporaryDirectory(prefix="present-aspect-") as tmp:
        unit, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        unit.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(exe)],
                       check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
