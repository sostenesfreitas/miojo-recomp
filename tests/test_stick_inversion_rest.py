"""Compile the real SDL axis pipeline: settings must not admit a held phantom stick (#2273).

Authored device samples exercise inversion, rest/crossing recovery, independent
devices, a phantom axis beside a real one, diagonals and every dead zone. No controller, game data or global input is used.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def function(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    pad = (args.source / "host/pad_sdl.c").read_text(encoding="utf-8")
    hid = (args.source / "host/nn_hid.c").read_text(encoding="utf-8")
    start = pad.index("            const int inv_x = rx_option(16, 0);")
    end = pad.index("            for (int k = 0; k < 4; k++)\n                if ((v[k]", start)
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { AX_LX, AX_LY, AX_RX, AX_RY };
static struct { unsigned rested; signed char held[4]; } pads[2];
static int options[20];
static int rx_option(int id, int fallback) { (void)fallback; return options[id]; }
static int16_t hos_sdl_pad_axis(void *gc, int axis) { return ((int16_t *)gc)[axis]; }
''' + function(hid, "void hos_filter_stick(int16_t x, int16_t y, int zone, int32_t out[2]) {") + r'''
static void poll(int i, int16_t raw[4], int32_t output[4]) {
    void *gc = raw;
''' + pad[start:end] + r'''
    memcpy(output, v, sizeof v);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
static int expected(int raw, int axis) {
    int value = raw < -32767 ? -32767 : raw;
    if (axis == AX_LY || axis == AX_RY) value = -value;
    if ((axis == AX_RX && options[16]) || (axis == AX_RY && options[17])) value = -value;
    return value;
}
int main(void) {
    const int cuts[6] = {7849, 1638, 3277, 4915, 6553, 9830};
    unsigned scenarios = 0;
    for (int zone = 0; zone < 6; zone++) for (int axis = AX_RX; axis <= AX_RY; axis++) {
        const int levels[4] = {32767, -32768, cuts[zone] + 1, -cuts[zone] - 1};
        for (unsigned level = 0; level < 4; level++) {
            int16_t raw[4] = {0}, neutral[4] = {0};
            int32_t output[4];
            memset(pads, 0, sizeof pads); memset(options, 0, sizeof options);
            options[11] = zone; raw[axis] = (int16_t)levels[level];
            /* Device 0 is stuck from connection; device 1 has actually rested. */
            poll(1, neutral, output);
            for (int n = 0; n < 12; n++) {
                options[16] = n & 1; options[17] = (n >> 1) & 1;
                poll(0, raw, output);
                CHECK(output[axis] == 0);
                poll(1, raw, output);
                CHECK(output[axis] == expected(raw[axis], axis));
            }
            /* Setting changes did not consume the real worn-stick crossing. */
            raw[axis] = raw[axis] < 0 ? 25000 : -25000;
            poll(0, raw, output);
            CHECK(output[axis] == expected(raw[axis], axis));
            raw[axis] = (int16_t)levels[level];
            poll(0, raw, output);
            CHECK(output[axis] == expected(raw[axis], axis));
            scenarios++;

            /* Neutral still admits an axis after any inversion history. */
            memset(pads, 0, sizeof pads);
            poll(0, raw, output); CHECK(output[axis] == 0);
            options[axis == AX_RX ? 16 : 17] ^= 1;
            poll(0, raw, output); CHECK(output[axis] == 0);
            raw[axis] = (int16_t)cuts[zone];
            poll(0, raw, output); CHECK(output[axis] == 0);
            raw[axis] = (int16_t)levels[level];
            poll(0, raw, output); CHECK(output[axis] == expected(raw[axis], axis));
            scenarios++;
        }
        /* Both right axes toggle independently; left axes retain their mapping. */
        memset(pads, 0, sizeof pads);
        int16_t raw[4] = {0}; int32_t output[4];
        poll(0, raw, output);
        raw[0] = 22000; raw[1] = -24000; raw[2] = -28000; raw[3] = 30000;
        for (int setting = 0; setting < 4; setting++) {
            options[16] = setting & 1; options[17] = (setting >> 1) & 1;
            poll(0, raw, output);
            for (int k = 0; k < 4; k++) CHECK(output[k] == expected(raw[k], k));
        }
        scenarios++;
    }
    for (int zone = 0; zone < 6; zone++) {
        /* A phantom axis held from connection does not carry its partner's drift past the circle, and the partner
           still rests at its own center (#1600). */
        for (int phantom = 0; phantom < 4; phantom++) {
            const int partner = phantom ^ 1;
            int16_t raw[4] = {0}; int32_t output[4];
            memset(pads, 0, sizeof pads); memset(options, 0, sizeof options);
            options[11] = zone;
            raw[phantom] = 32767; raw[partner] = (int16_t)(cuts[zone] / 2);
            poll(0, raw, output);
            CHECK(output[phantom] == 0 && output[partner] == 0);
            raw[partner] = (int16_t)(cuts[zone] + 1);
            poll(0, raw, output);
            CHECK(output[phantom] == 0 && output[partner] == expected(raw[partner], partner));
            raw[partner] = (int16_t)(-cuts[zone] / 2);
            poll(0, raw, output);
            CHECK(output[partner] == 0);
            scenarios++;
        }
        /* Diagonals keep both components whole: full tilt, and just past the circle while inside it on each axis. */
        const int16_t tilts[3] = {23170, 32767, (int16_t)(3 * cuts[zone] / 4)};
        for (int n = 0; n < 3; n++) for (int quadrant = 0; quadrant < 4; quadrant++) {
            int16_t raw[4] = {0}; int32_t output[4];
            memset(pads, 0, sizeof pads); memset(options, 0, sizeof options);
            options[11] = zone;
            poll(0, raw, output);
            for (int k = 0; k < 4; k++)
                raw[k] = (int16_t)(((k & 1) ? quadrant >> 1 : quadrant) & 1 ? -tilts[n] : tilts[n]);
            poll(0, raw, output);
            for (int k = 0; k < 4; k++) CHECK(output[k] == expected(raw[k], k));
            scenarios++;
        }
    }
    printf("PASS: %u axis/device/rest scenarios; all dead zones and inversion combinations\n", scenarios);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="stick-inversion-rest-") as folder:
        c = Path(folder) / "fixture.c"
        exe = Path(folder) / "fixture.exe"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
