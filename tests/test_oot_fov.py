"""Game-free FOV hook, camera ownership, live changes and menu wiring.

Compile the actual host helper and the lifter's injected call against authored
camera objects. No dump, lifted code or game assets are needed.
"""
import argparse
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_fov


class InstallTests(unittest.TestCase):
    def install(self, words=None, build_id=oot_fov.BUILD_ID, existing=None):
        class Lifter:
            after = {} if existing is None else dict(existing)

        class Image:
            def word(self, pc):
                return (oot_fov.WORDS if words is None else words)[(pc - oot_fov.SITE) // 4]

        lifter = Lifter()
        result = oot_fov.install(lifter, Image(), build_id)
        return result, lifter.after

    def test_supported_site_and_existing_hook(self):
        count, after = self.install(existing={oot_fov.SITE: "prior();"})
        self.assertEqual(count, 1)
        self.assertEqual(after[oot_fov.SITE], "prior();\n" + oot_fov.HOOK)
        self.assertIn("hos_oot_fov(c, c->x[19]);", after[oot_fov.SITE])

    def test_rejects_wrong_build_and_each_changed_instruction(self):
        self.assertEqual(self.install(build_id="another build"), (0, {}))
        for i in range(len(oot_fov.WORDS)):
            words = list(oot_fov.WORDS)
            words[i] ^= 1
            self.assertEqual(self.install(words=words), (0, {}))

    def test_lift_and_menu_wiring(self):
        lift = (ROOT / "nx2/lift.py").read_text(encoding="utf-8")
        self.assertLess(lift.index("oot_fov.install("), lift.index("L.discover("))
        menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
        api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
        options = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
        replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
        self.assertIn("HOS_OPT_FOV = 51", api)
        option_ids = {name: int(value) for name, value in re.findall(r"(HOS_OPT_FOV|HOS_OPT_PROMPTS|HOS_OPT_CAMERA_SPEED)\s*=\s*(\d+)", api)}
        self.assertEqual(len(option_ids), 3)
        self.assertEqual(len(set(option_ids.values())), 3, "FOV must not alias prompts or camera speed")
        self.assertIn("int fov = 100;", options)
        self.assertIn('"Field of view", &opts.fov, 50, 150, 5, tx_fov', menu)
        self.assertIn("if (id == HOS_OPT_FOV) return opts.fov;", replay)
        self.assertIn("if (id == HOS_OPT_PROMPTS) return opts.prompts;", replay)
        self.assertIn("if (id == HOS_OPT_CAMERA_SPEED) return opts.camera_speed;", replay)
        for name in ("is_slider", "stops_at_ends"):
            function = menu[menu.index("bool " + name):].split("\n", 1)[0]
            self.assertIn("&opts.fov", function)
            self.assertIn("&opts.camera_speed", function)


HEAD = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[32]; unsigned char vreg[32][16]; } Ctx;
enum { HOS_OPT_FOV = 51 };
static unsigned char objects[0x10000], globals[0x20000];
static int menu = 100, reads;
static void *ptr(uint64_t a, unsigned n) {
    if (a >= 0x10000000ULL && a + n <= 0x10010000ULL) return objects + (a - 0x10000000ULL);
    CHECK(a >= 0x84c90000ULL && a + n <= 0x84cb0000ULL);
    return globals + (a - 0x84c90000ULL);
}
static uint64_t recomp_load64(Ctx *c, uint64_t a) {
    uint64_t v; (void)c; reads++; memcpy(&v, ptr(a, 8), 8); return v;
}
static uint32_t recomp_load32(Ctx *c, uint64_t a) {
    uint32_t v; (void)c; reads++; memcpy(&v, ptr(a, 4), 4); return v;
}
static void a64_vset64(void *v, uint64_t bits) { memcpy(v, &bits, 8); memset((char *)v + 8, 0, 8); }
int rx_option(int id, int otherwise) { CHECK(id == HOS_OPT_FOV && otherwise == 100); return menu; }
'''

MAIN = r'''
static void site(Ctx *c) {
SITE_C
}
static void put64(uint64_t a, uint64_t v) { memcpy(ptr(a, 8), &v, 8); }
static void put32(uint64_t a, uint32_t v) { memcpy(ptr(a, 4), &v, 4); }
static const uint64_t camera = 0x10001000, views = 0x10002000, view = 0x10003000;
static const uint64_t singleton = 0x10004000, manager = 0x10005000, cameras = 0x10006000, main_camera = 0x10007000;
static const uint64_t events_singleton = 0x10008000, events = 0x10009000;
static Ctx input(float angle) {
    Ctx c; memset(&c, 0xab, sizeof c);
    c.x[19] = camera;
    uint32_t bits; memcpy(&bits, &angle, 4); a64_vset64(c.vreg[0], bits);
    return c;
}
static float output(Ctx *c) { float f; memcpy(&f, c->vreg[0], 4); return f; }
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }
static void unchanged(float angle) {
    Ctx c = input(angle), before = c; site(&c); CHECK(!memcmp(&c, &before, sizeof c));
}
static void setup(void) {
    put64(camera + 0x18, views); put64(views + 8, view); put64(view, 0x84c18a88ULL);
    put64(0x84ca6bc0ULL, singleton); put64(singleton, manager);
    put32(manager + 32, 1); put64(manager + 40, cameras); put64(cameras, main_camera);
    put64(main_camera + 120, view);
    put64(0x84c9e090ULL, events_singleton); put64(events_singleton, events);
    put64(events + 104, 0);
}
int main(void) {
    /* Original and unsupported choices touch no camera memory, preserve all bits. */
    unchanged(1.0f); unchanged(NAN); CHECK(reads == 0);
    for (int i = 0; i < 3; i++) { menu = (int[]){0, 49, 151}[i]; unchanged(1.0f); }
    CHECK(reads == 0);
    setup(); menu = 150;
    unsigned char before_objects[sizeof objects], before_globals[sizeof globals];
    memcpy(before_objects, objects, sizeof objects); memcpy(before_globals, globals, sizeof globals);
    for (int i = 0; i < 10; i++) {
        Ctx c = input(1.0f), expected = c;
        uint32_t bits; float wider = 1.5f; memcpy(&bits, &wider, 4); a64_vset64(expected.vreg[0], bits);
        site(&c); CHECK(!memcmp(&c, &expected, sizeof c));
    }
    /* Projection parameters are derived downstream; the authored angle is untouched.
       A wider angle admits a point outside the original frustum at both aspects. */
    Ctx c = input(1.0f); site(&c);
    for (int i = 0; i < 2; i++) {
        double aspect = i ? 32.0 / 9.0 : 16.0 / 9.0;
        double edge = tan(0.6) * aspect;
        CHECK(edge / (tan(0.5) * aspect) > 1.0);
        CHECK(edge / (tan(output(&c) / 2.0) * aspect) < 1.0);
    }
    menu = 50; c = input(1.0f); site(&c); CHECK(near(output(&c), 0.5f));
    menu = 125; c = input(0.4f); site(&c); CHECK(near(output(&c), 0.5f));   /* a zoom, from its current angle */
    menu = 100; unchanged(0.4f);   /* live reset, no cached scaled value */
    menu = 150;
    CHECK(!memcmp(objects, before_objects, sizeof objects) && !memcmp(globals, before_globals, sizeof globals));
    c = input(3.0f); site(&c); CHECK(near(output(&c), 2.443460953f));
    menu = 50; c = input(0.01f); site(&c); CHECK(near(output(&c), 0.087266463f));
    menu = 150; unchanged(NAN); unchanged(INFINITY); unchanged(0.0f); unchanged(-1.0f); unchanged(3.2f);

    /* Other views and scripted events retain their angles. Null links stop safely. */
    put64(main_camera + 120, view + 0x100); unchanged(1.0f); setup();
    put64(events + 104, 1); unchanged(1.0f); setup();
    put64(view, 0); unchanged(1.0f); setup();
    put64(camera + 0x18, 0); unchanged(1.0f); setup();
    put64(views + 8, 0); unchanged(1.0f); setup();
    put64(0x84ca6bc0ULL, 0); unchanged(1.0f); setup();
    put64(singleton, 0); unchanged(1.0f); setup();
    put32(manager + 32, 0); unchanged(1.0f); setup();
    put64(manager + 40, 0); unchanged(1.0f); setup();
    put64(cameras, 0); unchanged(1.0f); setup();
    c = input(1.0f); c.x[19] = 0; Ctx before = c; site(&c); CHECK(!memcmp(&c, &before, sizeof c));
    puts("PASS: live FOV, original reset, zoom, view ownership, events, bounds and register preservation");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(InstallTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    host = (ROOT / "host/oot_fov.c").read_text(encoding="utf-8")
    body = "".join(line for line in host.splitlines(keepends=True) if not line.startswith("#include"))
    with tempfile.TemporaryDirectory(prefix="oot-fov-") as temporary:
        source = Path(temporary) / "fixture.c"
        executable = Path(temporary) / "fixture.exe"
        source.write_text(HEAD + body + MAIN.replace("SITE_C", oot_fov.HOOK), encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-lm", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
