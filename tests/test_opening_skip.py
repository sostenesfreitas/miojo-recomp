"""Skip opening: only the new-game story movie and Navi waking Link (host/hos_main.c).

The host function is cut out of the source and compiled with stubs, so the check needs no game dump. It checks that
Opening_Introduction and Opening_LoadingKokiriForest are the only events skipped, that their timelines
Dm_Kokiri_0000 and Dm_Kokiri_0030 are the only ones not started, that Meet_DekuTree is not cancelled, that the
option defaults off, and that option id 44 stays Skip title's. The request is still sent. The cutscene frame
counter is not moved.

    python -I -B tests/test_opening_skip.py [--cc gcc]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


HARNESS = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Ctx;
static uint8_t mem[0x4000];
static char said[2048];
static int opt;
enum { HOS_OPT_SKIP_OPENING = %d };
static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return mem[a]; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) {
    uint64_t v; (void)c; memcpy(&v, mem + a, 8); return v;
}
static void recomp_store8(Ctx *c, uint64_t a, uint64_t v) { (void)c; mem[a] = (uint8_t)v; }
static void put64(uint64_t a, uint64_t v) { memcpy(mem + a, &v, 8); }
int rx_option(int id, int otherwise) { return id == HOS_OPT_SKIP_OPENING ? opt : otherwise; }
void hos_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(said + strlen(said), sizeof said - strlen(said), fmt, ap);
    va_end(ap);
    strncat(said, "\n", sizeof said - strlen(said) - 1);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %%d: %%s\n", __LINE__, #x); return 1; } } while (0)
/* FUNCTION */
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    memcpy(mem + 0x100, "Opening_Introduction", 21);
    memcpy(mem + 0x180, "Opening_LoadingKokiriForest", 28);
    memcpy(mem + 0x200, "Meet_DekuTree", 13);
    memcpy(mem + 0x280, "Dm_Kokiri_0000", 14);
    memcpy(mem + 0x300, "Dm_Kokiri_0030", 14);
    memcpy(mem + 0x380, "Dm_Kokiri_0070", 14);
    hos_oot_opening(0, 3);

    opt = 0;
    c.x[1] = 0x1000; put64(0x1000 + 6800, 0x100);
    CHECK(hos_oot_opening(&c, 0) == 0);
    CHECK(said[0] == 0);
    c.x[0] = 0x500; c.x[1] = 0x400; put64(0x400, 0x280); mem[0x500 + 0xf25] = 0xff;
    CHECK(hos_oot_opening(&c, 1) == 0);
    CHECK(mem[0x500 + 0xf25] == 0xff);

    opt = 1;
    c.x[1] = 0x1000; put64(0x1000 + 6800, 0x100);
    CHECK(hos_oot_opening(&c, 0) == 0);
    CHECK(strstr(said, "opening skip: Opening_Introduction request sent\n") != 0);
    said[0] = 0;
    CHECK(hos_oot_opening(&c, 0) == 0);
    CHECK(said[0] == 0);

    c.x[0] = 0x500; c.x[1] = 0x400; put64(0x400, 0x280); mem[0x500 + 0xf25] = 0xff;
    CHECK(hos_oot_opening(&c, 1) == 1);
    CHECK(mem[0x500 + 0xf25] == 0xfe);
    put64(0x400, 0x300); mem[0x500 + 0xf25] = 0xff;
    CHECK(hos_oot_opening(&c, 1) == 1);
    CHECK(mem[0x500 + 0xf25] == 0xfe);
    put64(0x400, 0x380); mem[0x500 + 0xf25] = 0xff;
    CHECK(hos_oot_opening(&c, 1) == 0);
    CHECK(mem[0x500 + 0xf25] == 0xff);

    c.x[0] = 7;
    CHECK(hos_oot_opening(&c, 2) == 1);
    CHECK(c.x[0] == 1);

    c.x[1] = 0x1000; put64(0x1000 + 6800, 0x200);
    CHECK(hos_oot_opening(&c, 0) == 0);
    CHECK(strstr(said, "Meet_DekuTree") == 0);
    c.x[0] = 7;
    CHECK(hos_oot_opening(&c, 2) == 0);
    CHECK(c.x[0] == 7);
    put64(0x400, 0x280); c.x[0] = 0x500; c.x[1] = 0x400; mem[0x500 + 0xf25] = 0xff;
    CHECK(hos_oot_opening(&c, 1) == 0);
    CHECK(mem[0x500 + 0xf25] == 0xff);

    put64(0x1000 + 6800, 0x180);
    c.x[1] = 0x1000;
    CHECK(hos_oot_opening(&c, 0) == 0);
    CHECK(strstr(said, "opening skip: Opening_LoadingKokiriForest request sent\n") != 0);
    put64(0x400, 0x300); c.x[0] = 0x500; c.x[1] = 0x400; mem[0x500 + 0xf25] = 1;
    CHECK(hos_oot_opening(&c, 1) == 1);
    CHECK(mem[0x500 + 0xf25] == 0);

    hos_oot_opening(&c, 3);
    c.x[0] = 7;
    CHECK(hos_oot_opening(&c, 2) == 0);
    CHECK(c.x[0] == 7);

    opt = 0;
    c.x[1] = 0x1000; put64(0x1000 + 6800, 0x100);
    CHECK(hos_oot_opening(&c, 0) == 0);
    opt = 1;
    put64(0x400, 0x280); c.x[0] = 0x500; c.x[1] = 0x400;
    CHECK(hos_oot_opening(&c, 1) == 0);
    puts("PASS");
    return 0;
}
"""


def compile_and_run(fn, cc, opt_id):
    text = HARNESS % opt_id
    text = text.replace("/* FUNCTION */", fn)
    # The %% in the CHECK macro survived one formatting pass as %.
    with tempfile.TemporaryDirectory(prefix="opening-skip-") as tmp:
        src = Path(tmp) / "opening_skip.c"
        exe = Path(tmp) / ("opening_skip.exe" if os.name == "nt" else "opening_skip")
        src.write_text(text, encoding="utf-8")
        cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        need(result.returncode == 0, "the sliced function did not compile:\n" + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        need(result.returncode == 0 and result.stdout.strip() == "PASS",
             "the sliced function did not behave:\n" + result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="")
    args = parser.parse_args()
    source = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    menu_h = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")

    fn = block(source, "int hos_oot_opening(Ctx *c, int mode) {")
    need('strcmp(name, "Opening_Introduction")' in fn, "Opening_Introduction is not a skipped event")
    need('strcmp(name, "Opening_LoadingKokiriForest")' in fn, "Opening_LoadingKokiriForest is not a skipped event")
    need(fn.count('strcmp(name, "Opening_') == 2, "an opening event besides those two is named")
    need('strcmp(name, "Dm_Kokiri_0000")' in fn and 'strcmp(name, "Dm_Kokiri_0030")' in fn,
         "the two opening timelines are not the ones left unstarted")
    need(fn.count('strcmp(name, "Dm_Kokiri_') == 2, "a timeline besides the two opening movies is named")
    need("Meet_DekuTree" in fn and "is not skipped" in fn, "Meet_DekuTree is not left as a later flow")
    need('strcmp(name, "Meet_DekuTree")' not in fn, "Meet_DekuTree is cancelled with the opening")
    need("return 0;   /* the request is still sent */" in fn, "the opening request is not sent on")
    need("0x2f8" not in fn and "a64_lookup" not in fn, "the skip moves the cutscene frame counter or calls a new guest")
    need("rx_option(HOS_OPT_SKIP_OPENING, 0)" in fn, "the option does not default off when the menu has not answered")
    need("HOS_OPT_SKIP_TITLE" not in fn and "44" not in fn, "the opening skip uses Skip title's id 44")

    need("44 is reserved for HOS_OPT_SKIP_TITLE" in api, "option id 44 is no longer reserved for Skip title")
    need(re.search(r"enum \{ HOS_OPT_SKIP_OPENING = 52 \};", api), "Skip opening is not option id 52")
    need("HOS_OPT_SKIP_OPENING = 44" not in api, "Skip opening took id 44")
    need("skip_opening" not in api, "Skip opening added to MkwVkHost or MkwVkRenderer (it is an option id, not an interface change)")

    need(re.search(r"int skip_opening = 0;", menu_h), "Skip opening does not default off")
    row = re.search(r'\{TAB_GAMEPLAY, "Skip opening", &opts\.skip_opening, 0, 1, 1, tx_onoff,', menu)
    need(row, "the Gameplay row is missing")
    help_at = menu.index("Skip opening")
    help_text = menu[help_at:help_at + 400]
    need("story movie and Navi waking Link" in help_text and "Deku Tree" in help_text and "title screen" in help_text,
         "the row's help does not say what is skipped and what is left")
    need("if (id == HOS_OPT_SKIP_OPENING) return opts.skip_opening;" in replay, "the option callback does not answer it")
    option = replay.split("t.option =", 1)[1].split("};", 1)[0]
    need(not re.search(r"\bid\s*==\s*44\b", option), "the option callback handles id 44")
    need('getenv("HOS_SKIP_OPENING")' in menu, "a run cannot turn the row on without saving it")

    lift = block(source, "void hos_oot_lift_player(Ctx *c, uint64_t actor) {")
    need("if (!oot_event_playing(c)) hos_oot_opening(c, 3);" in lift,
         "Link updating during the opening drops the skip before the flowchart finishes")

    fake = ('#include "a64.h"\n' + oot_patches.OPENING_REQUEST_HEAD + "    c->x[0] = 0;\n}\n"
            + oot_patches.OPENING_TIMELINE_HEAD + "    c->x[0] = 0;\n}\n"
            + oot_patches.OPENING_QUERY_HEAD + "    c->x[0] = 0;\n}\n")
    patched = oot_patches.patch_opening(fake)
    need(patched is not None, "the three opening functions were not patched")
    need(oot_patches.OPENING_REQUEST in patched and "return" not in oot_patches.OPENING_REQUEST,
         "the request hook swallows the request")
    need("return" in oot_patches.OPENING_TIMELINE and "return" in oot_patches.OPENING_QUERY,
         "the timeline or the skip query still runs its body")
    need(patched.count(oot_patches.OPENING_REQUEST) == 1, "the request hook was inserted more than once")
    need(oot_patches.patch_opening(patched) == patched, "patching a file again inserted the hooks again")
    need(oot_patches.patch_opening('#include "a64.h"\nvoid f_1(Ctx *c) {\n}\n') is None,
         "a file without the opening functions was patched")
    plain = oot_patches.patch_file(fake, "code_opening.c", False)
    need(oot_patches.OPENING_REQUEST not in plain[0], "another build's functions were hooked")
    built = oot_patches.patch_file(fake, "code_opening.c", True)
    need(built[1]["opening skip"] == 3, "the supported build did not count three opening hooks")

    cc = args.cc or shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
    if cc:
        compile_and_run(fn, cc, 52)
        print("PASS: skip opening leaves Meet_DekuTree and id 44, and the sliced function behaves")
    else:
        print("PASS: skip opening leaves Meet_DekuTree and id 44 (no C compiler here for the sliced function)")


if __name__ == "__main__":
    main()
