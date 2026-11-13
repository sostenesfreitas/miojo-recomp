"""HOS_ENV_FILE names the switches file a run reads instead of hookshot_env.txt beside the game.

A scripted or A/B test run brings its own switches this way, and the player's hookshot_env.txt is neither read nor
changed. Unset or empty, the game reads hookshot_env.txt as before. A named file that does not exist sets nothing: the
run does not fall back to the player's switches. A switch already in the environment the game was started with goes over
the file's line for it.

python -I -B tests/test_env_file.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    main_c = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    fn = block(main_c, "static int env_line_allowed(") + "\n" + block(main_c, "static void load_hookshot_env(void) {")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        game = tmp / "game"
        game.mkdir()
        # (the player's file sets the game's switches only: not the system's variables, HOS_ENV_FILE or paths)
        (game / "hookshot_env.txt").write_text(
            "HOS_T_BESIDE=1\nHOS_T_SHARED=beside\nVK_ADD_LAYER_PATH=/x\nLD_PRELOAD=/x.so\nPATH=C:\\x\nSDL_DYNAMIC_API=/x.so\n"
            "HOS_ENV_FILE=other.txt\nHOS_SAMPLE_FILE=/home/x/.bashrc\nRX_VK_DUMP_DIR=C:\\Users\\x\nRX_VK_SKIP_CODE=2c56,ab\n"
            "RX_VK_DLSS_DIR=/opt/ngx\nHOS_T_HOME=~\nSDL_VULKAN_LIBRARY=x.dll\nSDL_VIDEO_EGL_DRIVER=x.so\n"
            "SDL_AUDIODRIVER=pulseaudio\nHOS_T_UP=..\nHOS_SAMPLE_FILE=hookshot_vk.dll\nRX_VK_FRAME_LOG=x.txt\n"
            "RX_VK_DUMP_DIR=dumps\nHOS_PRESENT_TRACE=t.txt\nRX_VK_DLSS_DIR=\\\\server\\share\n"
            "HOS_AUDIO_DUMP=image.bin\nRX_VK_HANG_DUMP=hookshot_options.ini\nRX_VK_SHOTS=user\n"
            "HOS_INPUT_SCRIPT=1:A,2:A\nRX_VK_MENU_SCRIPT=1:INS\n", encoding="utf-8")
        (tmp / "test_env.txt").write_text("# a test run's own switches\nHOS_T_OTHER=1\nHOS_T_SHARED=other\n"
                                          "VK_T_LAYER=/y\n", encoding="utf-8")
        (tmp / "precedence.txt").write_text("HOS_T_LAUNCH=file\nHOS_T_TWICE=first\nHOS_T_FRESH=1\nHOS_T_TWICE=second\n",
                                            encoding="utf-8")
        exe = str(game / "hookshot").replace("\\", "\\\\")
        fixture = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32   /* (the test's own setenv/unsetenv: Windows has _putenv, which the game itself uses there) */
static int setenv(const char *name, const char *value, int overwrite) {
    char s[1024];
    (void)overwrite;
    snprintf(s, sizeof s, "%s=%s", name, value);
    return _putenv(s);
}
static int unsetenv(const char *name) { return setenv(name, "", 1); }
#endif
#define MAX_PATH 260
typedef unsigned long DWORD;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static DWORD hos_game_path_a(char *out, DWORD size) {   /* (host/hos_paths.h: the game folder's path) */
    snprintf(out, size, "%s", "''' + exe + r'''");
    return (DWORD)strlen(out);
}
''' + fn + r'''
static int is(const char *name, const char *want) {
    const char *v = getenv(name);
    return want ? v && !strcmp(v, want) : !v;
}
static void clear(void) { unsetenv("HOS_T_BESIDE"); unsetenv("HOS_T_OTHER"); unsetenv("HOS_T_SHARED"); unsetenv("VK_T_LAYER"); }
int main(int argc, char **argv) {
    (void)argc;
    unsetenv("HOS_ENV_FILE");                      /* out of the box: the file beside the game */
    load_hookshot_env();
    CHECK(is("HOS_T_BESIDE", "1") && is("HOS_T_SHARED", "beside") && is("HOS_T_OTHER", NULL));
    CHECK(is("VK_ADD_LAYER_PATH", NULL) && is("SDL_DYNAMIC_API", NULL) && is("HOS_ENV_FILE", NULL));
    CHECK(is("HOS_SAMPLE_FILE", NULL) && is("RX_VK_DUMP_DIR", NULL) && is("HOS_T_HOME", NULL));
    CHECK(is("HOS_AUDIO_DUMP", NULL) && is("RX_VK_HANG_DUMP", NULL) && is("RX_VK_SHOTS", NULL));
    CHECK(is("HOS_INPUT_SCRIPT", NULL) && is("RX_VK_MENU_SCRIPT", NULL));
    CHECK(is("RX_VK_SKIP_CODE", "2c56,ab") && is("RX_VK_DLSS_DIR", "/opt/ngx"));
    CHECK(is("SDL_VULKAN_LIBRARY", NULL) && is("SDL_VIDEO_EGL_DRIVER", NULL) && is("HOS_T_UP", NULL));
    CHECK(is("SDL_AUDIODRIVER", "pulseaudio") && is("HOS_SAMPLE_FILE", NULL) && is("RX_VK_FRAME_LOG", NULL));
    CHECK(is("RX_VK_DUMP_DIR", NULL) && is("HOS_PRESENT_TRACE", NULL));
    CHECK(getenv("LD_PRELOAD") == NULL || strcmp(getenv("LD_PRELOAD"), "/x.so"));
    CHECK(getenv("PATH") == NULL || strcmp(getenv("PATH"), "C:\\x"));

    clear();
    setenv("HOS_ENV_FILE", "", 1);                 /* empty: as if unset */
    load_hookshot_env();
    CHECK(is("HOS_T_BESIDE", "1") && is("HOS_T_OTHER", NULL));

    clear();
    setenv("HOS_ENV_FILE", argv[1], 1);            /* a test run's own file, and only that one */
    load_hookshot_env();
    CHECK(is("HOS_T_OTHER", "1") && is("HOS_T_SHARED", "other") && is("HOS_T_BESIDE", NULL) && is("VK_T_LAYER", "/y"));

    clear();
    setenv("HOS_ENV_FILE", argv[2], 1);            /* a file that is not there: nothing, not the player's switches */
    load_hookshot_env();
    CHECK(is("HOS_T_BESIDE", NULL) && is("HOS_T_OTHER", NULL) && is("HOS_T_SHARED", NULL));

    /* the environment the game was started with goes first: a switch set for one launch beats the file's line; the file's
       own later lines still replace its earlier ones */
    clear();
    setenv("HOS_T_LAUNCH", "launch", 1);
    setenv("HOS_ENV_FILE", argv[3], 1);
    load_hookshot_env();
    CHECK(is("HOS_T_LAUNCH", "launch") && is("HOS_T_TWICE", "second") && is("HOS_T_FRESH", "1"));
    load_hookshot_env();                           /* (read again in the same run: the same) */
    CHECK(is("HOS_T_LAUNCH", "launch") && is("HOS_T_TWICE", "second") && is("HOS_T_FRESH", "1"));
    printf("PASS: HOS_ENV_FILE replaces hookshot_env.txt; unset or empty reads it as before; the player's file sets only the game's switches; the environment goes over the file\n");
    return 0;
}
'''
        src = tmp / "fixture.c"
        src.write_text(fixture, encoding="utf-8")
        out = tmp / "fixture"
        subprocess.run([a.cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Werror", str(src), "-o", str(out)],
                       check=True)
        before = (game / "hookshot_env.txt").read_text(encoding="utf-8")
        subprocess.run([str(out), str(tmp / "test_env.txt"), str(tmp / "missing.txt"), str(tmp / "precedence.txt")], check=True)
        if (game / "hookshot_env.txt").read_text(encoding="utf-8") != before:
            raise SystemExit("the player's hookshot_env.txt was changed")


if __name__ == "__main__":
    main()
