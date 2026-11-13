"""When a retired pool's import is given back (gpu/vk/retired_give_back.h, used by vk_memory.cpp's
retired_imports_give_back): at once off Windows and on Windows NVIDIA, as before; after a wait on Windows AMD and Intel,
where retired pools were kept for the session and leaked. Compile and run the rule, and check vk_memory.cpp still goes
through it. No game or GPU is needed.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """vk_memory.cpp decides through the rule, reads its switches by the one rule for switches, and stamps a retirement"""
    src = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "retired_give_back.h"',
                   # (the default is by OS and vendor; a value of the switch is read as every on/off switch is)
                   'retired_give_back::mode(v && *v, env_on("RX_VK_KEEP_RETIRED"), windows, dev.props.vendorID == 0x10DE)',
                   "retired_give_back::due(mode, idle, idle_ms, floor_ms, want > 0)",
                   'env_int("RX_VK_RETIRE_SECS", 30)', 'env_int("RX_VK_RETIRE_MIN_SECS", 2)',
                   'env_int("RX_VK_IMPORT_HIGH", 85)', 'env_int("RX_VK_IMPORT_LOW", 75)',
                   # (the pressure is only asked about where retired imports wait: nowhere off Windows)
                   "if (mode == Mode::GRACE && high > 0) {",
                   "old->retired_ms = import_now_ms();",
                   # (what is queued is not asked for twice, by this valve or by imports_give_back's)
                   "retired_give_back::settled(used, dead_import_bytes), budget, high, low)",
                   "if (retired_mode() == retired_give_back::Mode::GRACE) used = retired_give_back::settled(used, dead_import_bytes);",
                   'static const int cap_mb = env_int("RX_VK_IMPORT_CAP_MB", -1);',
                   "const uint64_t cap = retired_give_back::live_budget(budget, (env_high && *env_high) || (env_low && *env_low), cap_mb);",
                   "const uint64_t want = retired_give_back::want(used, cap, high, low);",
                   "dead_import_bytes += p.hi - p.lo;",
                   "dead_import_bytes -= std::min(dead_import_bytes, dead_imports.front().bytes);"):
        if needle not in src:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)
    give = src[src.index("void retired_imports_give_back() {"):]
    give = give[:give.index("\n}\n")]
    for old in ("atoi(", 'getenv("RX_VK_KEEP_RETIRED")'):
        if old in give:
            raise SystemExit("retired_imports_give_back reads a switch the old way again: " + old)
    # (a retired pool whose mirror was refused has none either: its import goes too, or it stays pinned for the session)
    if "(p.mstate == Pool::MIRROR_NONE || p.mstate == Pool::MIRROR_REFUSED) && !p.inherit_pending" not in give:
        raise SystemExit("retired_imports_give_back keeps the import of a retired pool whose mirror was refused, for good")
    mode = src[src.index("retired_give_back::Mode retired_mode() {"):]
    mode = mode[:mode.index("\n}\n")]
    if "#ifdef _WIN32\n    constexpr bool windows = true;\n#else\n    constexpr bool windows = false;\n#endif" not in mode:
        raise SystemExit("retired_mode no longer tells Windows from the rest at build time")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="retired-give-back-") as tmp:
        exe = Path(tmp) / "retired-give-back"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/retired_give_back_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
