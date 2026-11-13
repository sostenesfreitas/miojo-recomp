"""A gyroscope that stops changing gets its sensors turned off and on again (host/pad_sdl.c motion_poll).

Seen with a Steam Controller started from Steam: after Alt+Tab out and back, Steam's layout switch left the pad's
sensors off while SDL still had them on, so the gyro read the same values for good until Motion controls was toggled.
The block from host/pad_sdl.c that notices this is compiled over a stand-in pad, then checked:

- readings that keep changing never turn the sensors off;
- the very same reading for 1.2 s or less does nothing (a pad that is simply still for a moment);
- the same reading for over 1.2 s turns both sensors off once and clears motion_on (the poll turns them on again);
- after that it waits 3 s before it would do it again, so a pad that stays frozen is not toggled every poll;
- no reading (motion off, no pad) does nothing.

python -I -B tests/test_gyro_rearm.py [--cc clang]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host/pad_sdl.c").read_text(encoding="utf-8")
    start = src.index("    /* (a gyroscope reading the very same values for over a second has stopped")
    start = src.index("    {", start)
    end = src.index("            same_since = 0;\n        }\n    }\n", start) + len("            same_since = 0;\n        }\n    }\n")
    block = src[start:end]
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { HOS_SDL_SENSOR_ACCEL = 1, HOS_SDL_SENSOR_GYRO = 2 };
static struct { void *gc; int motion_on; } pads[1];
static int offs, logs;
static int hos_sdl_pad_sensor_enable(void *gc, int sensor, int on) { (void)gc; (void)sensor; if (!on) offs++; return 1; }
static const char *hos_sdl_pad_name(void *gc) { (void)gc; return "Steam Controller"; }
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
static void step(int64_t now, int read, float gx) {
    const int pick = 0;
    float g[3] = {gx, 0.25f, -0.5f};
''' + block + r'''
}
#define MS 1000000LL
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    int dummy;
    pads[0].gc = &dummy, pads[0].motion_on = 1;
    int64_t t = 1000 * MS;
    /* changing readings: never */
    for (int n = 0; n < 1000; n++, t += 4 * MS) step(t, 1, (float)(n % 7) * 0.01f);
    CHECK(offs == 0 && pads[0].motion_on == 1);
    /* the same reading for 1.2 s: still nothing */
    int64_t from = t;
    for (; t - from <= 1200 * MS; t += 4 * MS) step(t, 1, 3.0f);
    CHECK(offs == 0 && pads[0].motion_on == 1);
    /* past 1.2 s: both sensors off once, motion_on cleared, one log line */
    for (int n = 0; n < 20; n++, t += 4 * MS) step(t, 1, 3.0f);
    CHECK(offs == 2 && pads[0].motion_on == 0 && logs == 1);
    /* still frozen: not again within 3 s */
    pads[0].motion_on = 1;
    from = t;
    for (; t - from < 2500 * MS; t += 4 * MS) step(t, 1, 3.0f);
    CHECK(offs == 2 && pads[0].motion_on == 1);
    /* frozen on past the 3 s wait: once more */
    for (int n = 0; n < 500; n++, t += 4 * MS) step(t, 1, 3.0f);
    CHECK(offs == 4 && logs == 2);
    /* no reading: nothing, however long */
    pads[0].motion_on = 1;
    for (int n = 0; n < 2000; n++, t += 4 * MS) step(t, 0, 3.0f);
    CHECK(offs == 4 && pads[0].motion_on == 1);
    printf("PASS: a frozen gyroscope's sensors are turned off and on again, at most every 3 s; a changing one never\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as d:
        c, exe = Path(d) / "gyro_rearm.c", Path(d) / "gyro_rearm.exe"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-o", str(exe), str(c)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr, end="")
            raise SystemExit(1)


if __name__ == "__main__":
    main()
