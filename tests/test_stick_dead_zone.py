"""The sticks' dead zone is the player's: the Insert menu's "Stick dead zone" row (stick_dead_zone in
hookshot_options.ini), issue 752.

SDL and XInput share a circular dead zone so diagonal stick inputs retain their direction (#1480).
The filter and the menu's row, labels and ini parsing are compiled alone (gcc, g++).

python -I -B tests/test_stick_dead_zone.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
THRESHOLDS = [7849, 1638, 3277, 4915, 6553, 9830]   # (Default, then 5% ... 30% of 32767)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def run(compiler, src, name, tmp, flags):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([compiler, *flags, "-Wall", "-Werror", "-o", str(exe), str(path)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    sdl = (ROOT / "host" / "pad_sdl.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")

    check(not re.search(r"HOS_OPT_\w+ = 11\b", api), "a HOS_OPT_ enum also takes option 11")
    pads = block(hid, "static void pads_read(")
    check("rx_option(11, 0)" in pads and "hos_filter_stick(thumb[0], thumb[1], zone, v)" in pads and
          "hos_filter_stick(thumb[2], thumb[3], zone, v + 2)" in pads,
          "XInput must filter both sticks with the menu's dead zone")
    poll = block(sdl, "static void pads_poll")
    check("rx_option(11, 0)" in poll and
          "hos_filter_stick(hos_sdl_pad_axis(gc, AX_LX), hos_sdl_pad_axis(gc, AX_LY), z, v)" in poll and
          "hos_filter_stick(hos_sdl_pad_axis(gc, AX_RX), hos_sdl_pad_axis(gc, AX_RY), z, v + 2)" in poll,
          "SDL must filter both sticks with the menu's dead zone")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    check("if (id == 11) return opts.stick_dead_zone;" in option, "the host cannot see the menu's row (option 11)")
    check("int stick_dead_zone = 0;" in header, "the option's default is not 0 (Default)")
    row = re.search(r'\{TAB_CONTROLS, "Stick dead zone", &opts\.stick_dead_zone, 0, 5, 1, tx_stick_dead_zone,\s*"[^}]*\},',
                    menu)
    check(row is not None, "the Insert menu has no Stick dead zone row (0 to 5)")
    check(row.group(0).rstrip().endswith("nullptr, false, true},"),
          "the row is not preset_exempt: changing the dead zone would set the quality preset to Custom")
    check("| | Stick dead zone | Default (24%), 5%, 10%, 15%, 20%, 30% | Default (24%) |" in readme,
          "readme.md's option table has no Stick dead zone row")

    c = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
''' + block(hid, "void hos_filter_stick(") + r'''
static void expect(int16_t x, int16_t y, int zone, int32_t want_x, int32_t want_y) {
    int32_t stick[2];
    hos_filter_stick(x, y, zone, stick);
    if (stick[0] != want_x || stick[1] != want_y) {
        fprintf(stderr, "zone %d: (%d, %d) became (%ld, %ld), wanted (%ld, %ld)\n",
                zone, x, y, (long)stick[0], (long)stick[1], (long)want_x, (long)want_y);
        exit(1);
    }
}

int main(void) {
    static const int32_t t[6] = {''' + ", ".join(map(str, THRESHOLDS)) + r'''};
    for (int z = 0; z < 6; z++) {
        /* On an axis, every input has the same threshold and output as before. */
        for (int32_t i = -32768; i <= 32767; i++) {
            const int32_t a = i < -32767 ? -32767 : i;
            const int32_t want = (a > t[z] || a < -t[z]) ? a : 0;
            expect((int16_t)i, 0, z, want, 0);
            expect(0, (int16_t)i, z, 0, want);
        }
        const int16_t inside = t[z] / 2, diagonal = 3 * t[z] / 4;
        expect(inside, -inside, z, 0, 0);
        expect(diagonal, diagonal, z, diagonal, diagonal);
        expect(-diagonal, diagonal, z, -diagonal, diagonal);
        expect(diagonal, -diagonal, z, diagonal, -diagonal);
        expect(-diagonal, -diagonal, z, -diagonal, -diagonal);
        expect(-32768, -32768, z, -32767, -32767);
        expect(23170, -23170, z, 23170, -23170);   /* (full tilt on a diagonal keeps its whole magnitude) */
        expect(-32767, 32767, z, -32767, 32767);
    }
    expect(12000, 4000, 0, 12000, 4000);   /* (the smaller component used to be zeroed) */
    expect(2949, 3932, 3, 0, 0);           /* (3-4-5 triangle exactly on the radius 4915) */
    expect(2949, 3933, 3, 2949, 3933);
    const int bad[] = {-1, 6, 99, -2147483647 - 1};
    for (int k = 0; k < 4; k++) {
        expect(7849, 0, bad[k], 0, 0);
        expect(7849, 1, bad[k], 7849, 1);
    }
    puts("stick: circular cutoff preserves direction and keeps all six thresholds");
    return 0;
}
'''

    init = block(menu, "void menu_init(")
    loop = block(init, "for (Row& r : rows) {")
    check("ini_key(r)" in loop, "menu_init's options loop no longer reads rows by ini_key")
    cxx = r'''#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
enum MenuTab { TAB_CONTROLS };
struct Options { int output_res = 0, vsync = 0, resolution = 0, fps_cap = 0, upscale = 0, dlss = 0, handheld = 0, ultrawide = 0, stick_dead_zone = 0, game_lang = 0; } opts;   // (ultrawide: the Aspect ratio row's key)
static int parse_output(const char* text) { return atoi(text); }   // (the Output row's own parse: not what this checks)
''' + block(menu, "struct Row {") + ";\n" + block(menu, "const char* tx_stick_dead_zone(") + "\n" + \
        "Row rows[] = {" + row.group(0) + "};\n" + block(menu, "const char* ini_key(") + r'''
static int parse(const char* text) {
    opts.stick_dead_zone = 0;
    char line[80];
    snprintf(line, sizeof line, "%s", text);
    char* eq = strchr(line, '=');
    *eq = 0;
    ''' + loop + r'''
    return opts.stick_dead_zone;
}
int main() {
    CHECK(!strcmp(ini_key(rows[0]), "stick_dead_zone"));
    CHECK(rows[0].preset_exempt && !rows[0].restart);
    CHECK(parse("stick_dead_zone=0\n") == 0 && parse("stick_dead_zone=3\n") == 3 && parse("stick_dead_zone=5\n") == 5);
    CHECK(parse("stick_dead_zone=9\n") == 5 && parse("stick_dead_zone=-4\n") == 0 && parse("stick_dead_zone=x\n") == 0);
    CHECK(parse("stick_deadzone=3\n") == 0);
    static const char* want[] = {"Default (24%)", "5%", "10%", "15%", "20%", "30%"};
    for (int v = 0; v < 6; v++) CHECK(!strcmp(rows[0].text(v), want[v]));
    CHECK(!strcmp(rows[0].text(-1), want[0]) && !strcmp(rows[0].text(6), want[0]));
    (void)parse_output("0");
    printf("menu: stick_dead_zone parses and clamps to 0..5; labels Default (24%%) ... 30%%\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="stick-dead-zone-") as tmp:
        run(a.cc, c, "stick.c", tmp, ["-std=c11", "-include", ENV_ON_H, "-O1"])
        run(a.cxx, cxx, "menu.cpp", tmp, ["-std=c++17", "-include", ENV_ON_H])
    print("ok")


if __name__ == "__main__":
    main()
