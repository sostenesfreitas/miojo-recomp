"""NVIDIA DLSS Frame Generation's runtime and its refusals (gpu/vk/vk_dlss.cpp, scripts/ngx_sdk.py), game-free.

The build placed only DLSS's runtime (nvngx_dlss.dll) in the game's ngx folder, so NGX took the driver's own copy of DLSS
Frame Generation's: on an RTX 5080 its 310.2.1 made 1 frame at most, so 3x and 4x were the renderer's own frames, and at 2x
it refused every set (0xbad00005), each refusal a wasted evaluation, the game at half its rate with nothing made.

Checked here:
- scripts/ngx_sdk.py fetches and installs frame generation's runtime beside DLSS's, under the name vk_dlss.cpp looks for;
- dlss_fg_frames gives frame generation up after kFgRefusalsOff refused sets in a row (as after three failed creations), a
  set made counting them again from 0, and says once when it is asked for more frames than its runtime makes;
- fg_start says whether the runtime folders hold frame generation's runtime or NGX takes the driver's own.

    python -I -B tests/test_dlssg_runtime.py
"""
import importlib.util
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def fail(why):
    raise SystemExit("FAIL: " + why)


def function(text, marker):
    start = text.index(marker)
    depth, at = 0, text.index("{", start)
    while True:
        depth += text[at] == "{"
        depth -= text[at] == "}"
        at += 1
        if depth == 0:
            return text[start:at]


def main():
    src = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    spec = importlib.util.spec_from_file_location("ngx_sdk_for_dlssg", ROOT / "scripts" / "ngx_sdk.py")
    ngx = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ngx)

    # (the runtime ngx_sdk.py installs is the one vk_dlss.cpp looks for, on both platforms)
    names = re.findall(r'constexpr const char\* kFgRuntime = "([^"]+)";', src)   # (Windows' first, then Linux')
    if len(names) != 2:
        fail("vk_dlss.cpp: kFgRuntime is not named for Windows and Linux")
    if ngx.runtime_name("windows", "framegen") != names[0]:
        fail("scripts/ngx_sdk.py installs %s, vk_dlss.cpp looks for %s" % (ngx.runtime_name("windows", "framegen"), names[0]))
    if not ngx.runtime_name("linux", "framegen").startswith(names[1]):
        fail("scripts/ngx_sdk.py installs %s, vk_dlss.cpp looks for %s*" % (ngx.runtime_name("linux", "framegen"), names[1]))
    if "framegen" not in ngx.RUNTIMES:
        fail("scripts/ngx_sdk.py does not install frame generation's runtime")

    off = re.search(r"constexpr int kFgRefusalsOff = (\d+);", src)
    if not off or not 1 < int(off.group(1)) <= 8:
        fail("vk_dlss.cpp: no kFgRefusalsOff (a few refused sets in a row before frame generation is given up)")
    fg = src[src.index("struct Fg {"):]
    if not re.search(r"\n    int refusals = 0;", fg[:fg.index("\n};")]):
        fail("Fg: no count of refused sets in a row")

    frames = function(src, "int dlss_fg_frames(")
    started = frames.index("if (!fg_start(g) || count < 1) return skip(dlss_why() != DLSS_OK ? FG_SKIP_DEVICE : FG_SKIP_START);")
    given_up = frames.find("if (g.refusals >= kFgRefusalsOff) {")
    create = frames.index("NGX_VK_CREATE_DLSSG(")
    if not started < given_up < create:
        fail("dlss_fg_frames: a set after frame generation was given up still goes to NGX")
    guard = frames[given_up:frames.index("}", given_up)]
    if "if (g.feature) fg_release(g);" not in guard or "return g.refused++, skip(FG_SKIP_REFUSED);" not in frames[given_up:given_up + 300]:
        fail("dlss_fg_frames: after giving up, the refused sets' feature is not let go (held for the rest of the run)")
    refused = frames.index("was refused (%#x)")
    off_at = frames.find("if (++g.refusals == kFgRefusalsOff) {", refused)
    if off_at < 0 or "g.start_failed = true;" not in frames[off_at:off_at + 200]:
        fail("dlss_fg_frames: refused sets in a row do not give frame generation up (start_failed: dlss_fg_runs says no)")
    loop_end = frames.find("    fg_barrier(cmd);\n    g.refusals = 0;")
    if not refused < loop_end < frames.index("g.made++;"):
        fail("dlss_fg_frames: a set made does not count the refusals again from 0")
    if not re.search(r"static bool told = false;.*?made frames asked, but its runtime makes %u at most"
                     r".*?return g\.too_many\+\+, skip\(FG_SKIP_TOO_MANY\);", frames, re.S):
        fail("dlss_fg_frames: more frames asked than its runtime makes is not said (once)")

    # (the Insert menu's Frame generation by row says why DLSS-G did not make the frames: each set it does not make names its
    # reason, one it makes clears it, and one with nothing to blend from leaves it, dlss_fg_skip)
    for line, why in (("if (!fg_possible()) return ", "FG_SKIP_OFF"), ("if (g.inputs == g.used || !g.rw) return ", "FG_SKIP_INPUTS")):
        at = frames.find(line)
        if at < 0 or ("skip(%s)" % why) not in frames[at:frames.index("\n", at)]:
            fail("dlss_fg_frames: a set not made for %s does not say so (dlss_fg_skip)" % why)
    if "if (!blend) return g.not_blended++, 0;" not in frames:
        fail("dlss_fg_frames: a set with nothing to blend from (a loading hitch) changes the reason the menu shows")
    if not re.search(r"g\.made\+\+;.*?skip\(FG_SKIP_NONE\);\s*return count;", frames, re.S):
        fail("dlss_fg_frames: a set DLSS-G made does not clear the reason the menu shows")
    skip_at = function(src, "int dlss_fg_skip(unsigned* most) {")
    if "fg_skip.load(" not in skip_at or "fg_skip_most.load(" not in skip_at:
        fail("dlss_fg_skip: does not give the last set's reason and its runtime's most")

    start = function(src, "bool fg_start(Fg& g) {")
    if "runtime_dirs(game_dir())" not in start or "kFgRuntimeName" not in start or "the driver's own" not in start:
        fail("fg_start: does not say whether frame generation's runtime is in the runtime folders or the driver's own is used")
    if "dlss_checks::ascii(fs::canonical(" not in start or "not all ASCII" not in start:
        fail("fg_start: on Linux a runtime only in a folder whose path is not ASCII is said to be used (NGX does not load it)")
    print("DLSS Frame Generation: its runtime installed and looked for by one name; given up after %s refused sets in a row"
          % off.group(1))


if __name__ == "__main__":
    sys.exit(main())
