"""A Debug row read before the menu loads (gpu/vk/vk_draw.cpp menu_env_on): Descriptor reuse on by default from options_version 4.

The real menu_env_on and desc_reuse_level from gpu/vk/vk_draw.cpp are compiled over a stub game path (the fixture's
folder); each case writes a hookshot_options.ini (or none) and checks the level the game takes for descriptor reuse
(2 the row's On, on by default since options_version 4: an older file wrote its 0 as the off default it then was; 1 its
Off; 0 never) and sampler_cache (off by default, no version rule):

- no file: the default; a file without the line: the default;
- options_version 3 with descriptor_reuse=0: on (the old default, moved); 4 with 0: off (chosen); 4 with 1: on;
- RX_VK_DESC_REUSE over all of it, with the row's values (0/off its Off, 1 its On) and never.

python -I -B tests/test_menu_env_on.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


FIXTURE = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "hookshot_env.h"
using DWORD = unsigned long;
constexpr int MAX_PATH = 260;
static DWORD game_module_path(char *path, DWORD size) { return snprintf(path, size, "./hookshot.exe"); }
static void logf(const char*, ...) {}
BODY
LEVEL
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const int reuse = desc_reuse_level();
    const bool sampler = menu_env_on("RX_VK_SAMPLER_CACHE", "sampler_cache");
    if (reuse != atoi(argv[1]) || sampler != (atoi(argv[2]) != 0)) {
        fprintf(stderr, "FAIL: descriptor reuse level %d (want %s), sampler_cache %d (want %s)\n", reuse, argv[1], sampler, argv[2]);
        return 1;
    }
    return 0;
}
"""

# (hookshot_options.ini's text or None for no file, RX_VK_DESC_REUSE or None, descriptor reuse level, sampler cache)
CASES = (
    (None, None, 2, 0),
    ("output_res=1\noptions_version=4\n", None, 2, 0),
    ("descriptor_reuse=0\nsampler_cache=0\noptions_version=3\n", None, 2, 0),
    ("descriptor_reuse=0\nsampler_cache=1\n", None, 2, 1),
    ("descriptor_reuse=0\nsampler_cache=1\noptions_version=4\n", None, 1, 1),
    ("options_version=4\ndescriptor_reuse=1\n", None, 2, 0),
    ("descriptor_reuse=0\noptions_version=4\n", "1", 2, 0),
    ("descriptor_reuse=1\noptions_version=4\n", "0", 1, 0),
    ("descriptor_reuse=1\noptions_version=4\n", "off", 1, 0),
    (None, "0", 1, 0),
    (None, "never", 0, 0),
    ("descriptor_reuse=1\noptions_version=4\n", "never", 0, 0),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    body = block(draw, "bool menu_env_on(const char* env_name, const char* ini_key_name, bool def = false, int since = 0) {")
    level = block(draw, "int desc_reuse_level() {")
    with tempfile.TemporaryDirectory(prefix="menu-env-on-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(FIXTURE.replace("BODY", body).replace("LEVEL", level), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-I", str(ROOT / "gpu/vk/compat"), str(source), "-o", str(exe)], check=True)
        for text, env_value, reuse, sampler in CASES:
            ini = work / "hookshot_options.ini"
            if ini.exists():
                ini.unlink()
            if text is not None:
                ini.write_text(text, encoding="ascii")
            env = os.environ.copy()
            env.pop("RX_VK_DESC_REUSE", None)
            env.pop("RX_VK_SAMPLER_CACHE", None)
            if env_value is not None:
                env["RX_VK_DESC_REUSE"] = env_value
            subprocess.run([str(exe), str(reuse), str(sampler)], cwd=work, env=env, check=True)
    print("PASS: Descriptor reuse on by default and for options files before version 4; a version 4 choice kept; "
          "RX_VK_DESC_REUSE with the row's values, and never; Sampler cache unchanged")


if __name__ == "__main__":
    main()
