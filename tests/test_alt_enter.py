"""Alt+Enter toggles fullscreen and presses no button (Enter is Plus by default, and any button can be bound to it; issue #390).

The bound keys' helper (host/nn_hid.c's kmap_down) is compiled alone against fake key states; the window
procedures (host/display.c, host/posix/display_sdl.c) are checked to toggle on Alt+Enter, once per press, and to
leave other Alt keys (Alt+F4) to Windows.

python -I -B tests/test_alt_enter.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def case(text, label):
    """The body of one `case label:` of a switch, up to the next case"""
    start = text.index("case %s:" % label)
    end = text.index("    case ", start + 1)
    return text[start:end]


def check_windows():
    src = (ROOT / "host" / "display.c").read_text(encoding="utf-8")
    sys_down = case(src, "WM_SYSKEYDOWN")
    for need, why in (("VK_RETURN", "Enter"), ("1 << 29", "the Alt bit"), ("1 << 30", "a held key repeating"),
                      ("set_fullscreen(w, !fullscreen)", "the toggle"), ("break;", "other Alt keys to DefWindowProc")):
        if need not in sys_down:
            raise SystemExit("display.c: WM_SYSKEYDOWN lacks %s (%s)" % (need, why))
    if "'\\r'" not in case(src, "WM_SYSCHAR"):
        raise SystemExit("display.c: Alt+Enter's WM_SYSCHAR is not taken (Windows beeps)")


def check_linux():
    src = (ROOT / "host" / "posix" / "display_sdl.c").read_text(encoding="utf-8")
    if "scancode == 40 && (mod & 0x300)" not in src:
        raise SystemExit("display_sdl.c: Alt+Enter no longer toggles fullscreen")


FIXTURE = r'''#include <stdio.h>
#include <stdlib.h>
#define VK_RETURN 0x0D
#define VK_MENU 0x12
#define VK_TAB 0x09
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static int held[256];
static int down(int vk) { return held[vk & 255]; }
@KMAP_DOWN@
int main(void) {
    held[VK_RETURN] = 1;
    CHECK(kmap_down(VK_RETURN, down));            /* Enter alone is Plus */
    held[VK_MENU] = 1;
    CHECK(!kmap_down(VK_RETURN, down));           /* Alt+Enter is not */
    held[VK_TAB] = 1;
    CHECK(kmap_down(VK_TAB, down));               /* Alt does not hide the other keys */
    held[VK_RETURN] = 0;
    CHECK(!kmap_down(VK_RETURN, down));
    held[VK_MENU] = 0;
    CHECK(!kmap_down(VK_MENU, down) && kmap_down(VK_TAB, down));
    printf("PASS: Alt+Enter is fullscreen, not Plus\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_windows()
    check_linux()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    fn = block(hid, "static int kmap_down(")
    if "kmap_down(key_bind[i][k], key)" not in hid:
        raise SystemExit("nn_hid.c: the bound keys no longer go through kmap_down")
    with tempfile.TemporaryDirectory(prefix="alt-enter-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(FIXTURE.replace("@KMAP_DOWN@", fn), encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
