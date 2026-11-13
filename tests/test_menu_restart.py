"""The Insert menu's rows that take a restart: what is pending and when closing the menu asks (gpu/vk/menu_restart.h,
by behaviour in tests/test_menu_restart.cpp), the menu's use of it, and the path from Restart now to a new instance: the
window closes as the player closes it (host/display.c, host/posix/display_sdl.c), then host/hos_relaunch.c starts the
game again. The command it starts (host/hos_relaunch.h) is checked against this test's own /proc/self files and made-up
ones. No game or GPU required.

python -I -B tests/test_menu_restart.py [--cxx g++] [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++", "--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

RELAUNCH_TEST = r"""
#include "hos_relaunch.h"
#include <assert.h>

static void write_file(const char *path, const char *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    fwrite(bytes, 1, n, f);
    fclose(f);
}
int main(int argc, char **argv) {
    const char *dir = argv[1];
    char a[1024], e[1024];
    snprintf(a, sizeof a, "%s/cmdline", dir), snprintf(e, sizeof e, "%s/environ", dir);
    /* (an empty argument kept, the last string ended by its NUL or not) */
    static const char args[] = "./bin/hookshot\0--user\0\0last";
    static const char env[] = "WAYLAND_DISPLAY=wayland-1\0XDG_DATA_HOME=/tmp/x\0";
    write_file(a, args, sizeof args - 1), write_file(e, env, sizeof env - 1);
    HosRelaunch r;
    assert(hos_relaunch_command(a, e, &r));
    assert(r.argc == 4 && !strcmp(r.argv[0], "./bin/hookshot") && !strcmp(r.argv[1], "--user") && !strcmp(r.argv[2], "") &&
           !strcmp(r.argv[3], "last") && !r.argv[4]);
    assert(r.envc == 2 && !strcmp(r.envp[0], "WAYLAND_DISPLAY=wayland-1") && !strcmp(r.envp[1], "XDG_DATA_HOME=/tmp/x") &&
           !r.envp[2]);
    /* a long command line (more than one read) */
    static char big[20000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = i % 100 == 99 ? 0 : 'a';
    write_file(a, big, sizeof big);
    assert(hos_relaunch_command(a, e, &r) && r.argc == 200 && strlen(r.argv[199]) == 99);
    /* no arguments, or no file: nothing to start */
    write_file(a, "", 0);
    assert(!hos_relaunch_command(a, e, &r));
    snprintf(a, sizeof a, "%s/missing", dir);
    assert(!hos_relaunch_command(a, e, &r));
    /* an empty environment is still one */
    snprintf(a, sizeof a, "%s/cmdline", dir);
    write_file(a, "x", 1), write_file(e, "", 0);
    assert(hos_relaunch_command(a, e, &r) && r.argc == 1 && r.envc == 0 && !r.envp[0]);
    /* this process's own: its arguments and its environment as it started */
    assert(hos_relaunch_command("/proc/self/cmdline", "/proc/self/environ", &r));
    assert(r.argc == argc);
    for (int i = 0; i < argc; i++) assert(!strcmp(r.argv[i], argv[i]));
    int found = 0;
    for (int i = 0; i < r.envc; i++) found |= !strcmp(r.envp[i], "HOS_RELAUNCH_TEST=one two");
    assert(found);
    /* Windows' restart: the environment block the game started with, HOS_RESTART_OF put in its place by name (case left
       out), the old one dropped; =C: and the like stay first */
    {
        static const wchar_t saved[] = L"=C:=C:\\games\0ALLUSERSPROFILE=a\0hos_restart_of=1\0RX_VK_PACE=0\0Zed=z\0";
        static const wchar_t *const want[] = {L"=C:=C:\\games", L"ALLUSERSPROFILE=a", L"HOS_RESTART_OF=42", L"RX_VK_PACE=0", L"Zed=z"};
        wchar_t *b = hos_relaunch_env_block(saved, L"HOS_RESTART_OF", L"42");
        const wchar_t *e = b;
        for (size_t i = 0; i < sizeof want / sizeof *want; i++, e += wcslen(e) + 1) assert(!wcscmp(e, want[i]));
        assert(!*e);
        free(b);
        /* last in the order, and an empty block */
        static const wchar_t two[] = L"A=1\0B=2\0";
        b = hos_relaunch_env_block(two, L"HOS_RESTART_OF", L"7");
        assert(!wcscmp(b, L"A=1") && !wcscmp(b + 4, L"B=2") && !wcscmp(b + 8, L"HOS_RESTART_OF=7") && !b[25]);
        free(b);
        b = hos_relaunch_env_block(L"", L"HOS_RESTART_OF", L"7");
        assert(!wcscmp(b, L"HOS_RESTART_OF=7") && !b[17]);
        free(b);
    }
    puts("relaunch command: all checks passed");
    return 0;
}
"""


def function(source, start):
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start)
    body = source[at:]
    return body[:body.index("\n}\n")]


def sources():
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    for row in ('"GPU device", &opts.gpu_device', '"Output resolution", &opts.output_res', '"Profile", &opts.handheld',
                '"Aspect ratio", &opts.ultrawide', '"Robust buffers", &opts.robust'):
        at = menu.find(row)
        if at < 0:
            raise SystemExit("vk_menu.cpp: a row that takes a restart is missing: " + row)
        entry = menu[at:menu.index("},", at)]
        if not re.search(r",\s*true$", entry):
            raise SystemExit("vk_menu.cpp: %s is not marked as taking a restart" % row.split(",")[0])
    # (the Profile row was "Handheld profile": its key stays the one host/nn_applet.c reads, and its values say Docked and Handheld)
    if not re.search(r'&opts\.handheld\)\s*return\s*"handheld_profile"', function(menu, "const char* ini_key(const Row& r) {")):
        raise SystemExit("ini_key: the Profile row is not saved as handheld_profile (host/nn_applet.c would not read it)")
    if 'return v ? "Handheld" : "Docked";' not in menu:
        raise SystemExit("vk_menu.cpp: the Profile row does not show Docked and Handheld")
    if "(restart)" in menu:
        raise SystemExit("vk_menu.cpp: a value still says (restart) whatever runs: \"(after restart)\" is added while it differs")
    if '"After restart"' not in menu or "Restart needed: " not in menu:
        raise SystemExit("vk_menu.cpp: no After restart tag on a changed value, or no Restart needed line")
    if "restart_init();" not in function(menu, "void menu_init() {"):
        raise SystemExit("menu_init does not keep the start values")
    if "restart_state.note(" not in function(menu, "void set_row("):
        raise SystemExit("set_row does not tell the restart state of a change")
    tick = function(menu, "void menu_tick() {")
    for need in ("restart_ask()", "choose_later(", "menu_hit::kButton", "PostMessageA(app_window(), WM_APP + 9", "save();",
                 "pipeline_cache_save(true)"):
        if need not in tick:
            raise SystemExit("menu_tick: the restart question lacks " + need)
    win = (ROOT / "host" / "display.c").read_text(encoding="utf-8")
    sdl = (ROOT / "host" / "posix" / "display_sdl.c").read_text(encoding="utf-8")
    close = win[win.index("case WM_APP + 9:"):]
    close = close[:close.index("return 0;")]
    if "hos_relaunch_ask();" not in close or "case WM_CLOSE:" not in close or "hos_window_closed_exit();" not in close:
        raise SystemExit("display.c: WM_APP + 9 does not close the window as WM_CLOSE does, with the restart asked")
    quit_ = sdl[sdl.index("case HOS_SDL_EV_QUIT:"):]
    if "hos_window_closed_exit();" not in quit_[:quit_.index("case HOS_SDL_EV_RESIZED:")]:
        raise SystemExit("display_sdl.c: closing the window does not go through hos_window_closed_exit")
    if "WM_APP + 9" not in sdl or "hos_relaunch_ask();" not in sdl:
        raise SystemExit("display_sdl.c: no WM_APP + 9")
    relaunch = (ROOT / "host" / "hos_relaunch.c").read_text(encoding="utf-8")
    end = function(relaunch, "void hos_window_closed_exit(void) {")
    if end.index("hos_log_flush();") > end.index("relaunch();") or end.index("relaunch();") > end.index("ExitProcess(0);"):
        raise SystemExit("hos_relaunch.c: the restart is not the last step before the exit (after the log)")
    for need in ("GetModuleFileNameW(", "GetCommandLineW()", "CreateProcessW(", '"/proc/self/cmdline"', '"/proc/self/environ"',
                 'execve("/proc/self/exe"'):
        if need not in relaunch:
            raise SystemExit("hos_relaunch.c: no " + need)
    # (Windows: the new process gets the environment the game started with, not this one's with hookshot_env.txt's lines in
    # it, or a restart takes each of the file's lines for the environment's and leaves the file's edits out)
    windows = relaunch[relaunch.index("static void relaunch(void) {"):relaunch.index("#else")]
    if not re.search(r"CreateProcessW\([^;]*CREATE_UNICODE_ENVIRONMENT[^;]*, env,", windows) or \
            "hos_relaunch_env_block(start_env, L\"HOS_RESTART_OF\"" not in windows:
        raise SystemExit("hos_relaunch.c: Windows' restart does not start the game with the environment it started with")
    if "GetEnvironmentStringsW()" not in function(relaunch, "void hos_relaunch_keep_env(void) {"):
        raise SystemExit("hos_relaunch.c: hos_relaunch_keep_env does not keep the environment block")
    hos_main = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    keep, load = hos_main.find("hos_relaunch_keep_env();"), hos_main.find("load_hookshot_env();")
    if keep < 0 or keep > load:
        raise SystemExit("hos_main.c: the environment is not kept before hookshot_env.txt is read")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    sources()
    with tempfile.TemporaryDirectory(prefix="menu-restart-") as tmp:
        exe = Path(tmp) / "menu-restart"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_menu_restart.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        if sys.platform.startswith("linux"):
            src = Path(tmp) / "relaunch_test.c"
            src.write_text(RELAUNCH_TEST, encoding="utf-8")
            rexe = Path(tmp) / "relaunch-test"
            subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(src), "-o", str(rexe)],
                           check=True)
            subprocess.run([str(rexe), tmp, "with an argument", ""], check=True,
                           env=dict(os.environ, HOS_RELAUNCH_TEST="one two"))
    print("PASS: the restart rows and Restart now")


if __name__ == "__main__":
    main()
