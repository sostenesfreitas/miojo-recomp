"""RX_VK_RECORDER=thread's two opt-in switches, RX_VK_RECORDER_OWN and RX_VK_RECORDER_IDLE=wait. No game, no GPU, no Vulkan library.

python -I -B tests/test_recorder_own_idle.py --headers <Vulkan-Headers/include> [--cxx clang++]

1. The sources: rec::OwnSlot marks the slot's own calls in vk_replay.cpp's begin_recording and vk_draw.cpp's draw_begin_recording
   and nowhere else (the frame's command buffer, ImGui, NGX and FSR keep their waits); begin_recording calls slot_begin() before
   them and slot_done() after draw_begin_recording; submit_recording keeps the slot's end position after its two ends. Controls: an
   OwnSlot in another function, and slot_begin() after the OwnSlot, are found.
2. tests/recorder_own_idle_test.cpp, built with the real vk_work.h and vk_recorder.h and a fake Vulkan, the recorder thread held in
   a gated packet (no timing: a 3 s watchdog turns a wrong wait into a failure):
   - the switches read as the renderer reads them, and named in the recorder's log line;
   - RX_VK_RECORDER_OWN=1: the slot's descriptor pool reset within OwnSlot reaches the driver while a packet is in flight; a call
     that is not the slot's own (vkEndCommandBuffer) within it, the reset outside it, the switch off (unset or 0), and a slot whose
     last end the recorder has not run (the guard, logged once) all wait for the packet;
   - RX_VK_RECORDER_IDLE: unset, WaitOnAddress is never called; =wait without the functions, turned off and logged at the start;
     the recorder waiting on W is woken by a publish; a publish between sleeping set and the wait (the wake made to do nothing)
     makes the wait return at once on the changed W.
3. The test has teeth: vk_recorder.h with vkEndCommandBuffer among the slot's own calls, the guard removed, the switch ignored,
   OwnSlot ignoring slot_begin(), the wait's compare taken from W instead of the r it last saw, or the wake removed, must fail it.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_recorder_sync as recsync  # noqa: E402  (its test_loader() and the paths)

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = recsync.ROOT
VK = recsync.VK


def body(text, head, what):
    """the function that starts with `head`, braces matched"""
    if text.count(head) != 1:
        raise SystemExit("%s: the anchor %r must appear once" % (what, head))
    at = text.index(head) + len(head)
    depth = 1
    start = at
    while depth:
        depth += {"{": 1, "}": -1}.get(text[at], 0)
        at += 1
    return text[start:at]


BEGIN = "Recording& begin_recording(uint64_t value) {"
SUBMIT = "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0) {"
DRAW_BEGIN = "void draw_begin_recording(int slot, VkCommandBuffer cmd, uint64_t value) {"


def source_problems(replay, draw, others):
    """what is wrong with OwnSlot's placement; others: {name: text} of the renderer's other files"""
    p = []
    begin, submit, draw_begin = body(replay, BEGIN, "vk_replay.cpp"), body(replay, SUBMIT, "vk_replay.cpp"), body(draw, DRAW_BEGIN, "vk_draw.cpp")
    if replay.count("rec::OwnSlot") != 1 or begin.count("rec::OwnSlot") != 1:
        p.append("vk_replay.cpp: rec::OwnSlot must appear once, in begin_recording")
    if draw.count("rec::OwnSlot") != 1 or draw_begin.count("rec::OwnSlot") != 1:
        p.append("vk_draw.cpp: rec::OwnSlot must appear once, in draw_begin_recording")
    for name, text in others.items():
        if "OwnSlot" in text:
            p.append("%s: OwnSlot outside the slot's own calls" % name)
    if not (0 <= begin.find("rec::slot_begin(") < begin.find("rec::OwnSlot")):
        p.append("begin_recording: rec::slot_begin() must come before the OwnSlot")
    if not (0 <= begin.find("draw_begin_recording(") < begin.find("rec::slot_done();")):
        p.append("begin_recording: rec::slot_done() must come after draw_begin_recording")
    own, at = "", begin.rfind("{", 0, begin.find("rec::OwnSlot"))   # (the block the OwnSlot is in, braces matched)
    if at >= 0:
        depth, i = 0, at
        while i < len(begin):
            depth += {"{": 1, "}": -1}.get(begin[i], 0)
            i += 1
            if not depth:
                break
        own = begin[at:i]
    for call in ("vkResetCommandPool(dev.device, r.pool", "vkResetCommandPool(dev.device, r.upload_pool", "vkBeginCommandBuffer(r.cmd",
                 "vkBeginCommandBuffer(r.upload"):
        if call not in own:
            p.append("begin_recording: %s not within the OwnSlot" % call)
    for call in ("gpu_time_begin", "texture_begin_recording", "draw_begin_recording"):
        if call in own:
            p.append("begin_recording: %s within the OwnSlot (only the slot's own resets and begins)" % call)
    ends = [m.start() for m in re.finditer(r"vkEndCommandBuffer\(r\.(cmd|upload)\);", submit)]
    keep = submit.find("rec_end_pos[r.slot] = rec::produced();")
    if len(ends) != 2 or keep < max(ends):
        p.append("submit_recording: rec_end_pos[r.slot] = rec::produced() must come after both ends")
    return p


def test_sources():
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    others = {f.name: f.read_text(encoding="utf-8", errors="replace") for f in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h"))
              if f.name not in ("vk_replay.cpp", "vk_draw.cpp", "vk_recorder.h")}
    problems = source_problems(replay, draw, others)
    if problems:
        raise SystemExit("sources:\n  " + "\n  ".join(problems))
    # (controls: the check finds an OwnSlot elsewhere, and slot_begin() after it)
    stray = dict(others, **{"vk_menu.cpp": others.get("vk_menu.cpp", "") + "\nvoid f() { rec::OwnSlot own; }\n"})
    if not source_problems(replay, draw, stray):
        raise SystemExit("sources: the control with an OwnSlot in vk_menu.cpp passed")
    begin = body(replay, BEGIN, "vk_replay.cpp")
    late = begin.replace("    rec::slot_begin(rec_end_pos[r.slot]);", "", 1).replace("gpu_time_begin(r);", "gpu_time_begin(r);\n    rec::slot_begin(rec_end_pos[r.slot]);", 1)
    if late == begin or not source_problems(replay.replace(begin, late, 1), draw, others):
        raise SystemExit("sources: the control with slot_begin() after the OwnSlot passed")
    print("PASS sources: OwnSlot only at the slot's own resets and begins, after slot_begin(); the end position kept after both ends")


def build(cxx, headers, out, recorder_text, name):
    d = out / name
    d.mkdir()
    (d / "vk_loader.h").write_text(recsync.test_loader(), encoding="utf-8")
    (d / "vk_work.h").write_text((VK / "vk_work.h").read_text(encoding="utf-8"), encoding="utf-8")
    (d / "vk_recorder.h").write_text(recorder_text, encoding="utf-8")
    (d / "gpu_wait_stats.h").write_text((VK / "gpu_wait_stats.h").read_text(encoding="utf-8"), encoding="utf-8")
    exe = d / "recorder_own_idle_test.exe"
    cmd = [cxx, "-std=c++20", "-include", recsync.ENV_ON_H, "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
           "-Wno-missing-field-initializers", "-DVK_NO_PROTOTYPES", "-D_CRT_SECURE_NO_WARNINGS", "-DRX_REC_TEST_HOOKS",
           "-I" + str(d), "-I" + str(headers), str(ROOT / "tests" / "recorder_own_idle_test.cpp"), "-o", str(exe)]
    if os.name != "nt":
        cmd.append("-pthread")
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    return exe


SWITCHES = ("RX_VK_RECORDER", "RX_VK_PARALLEL", "RX_VK_RECORDER_OWN", "RX_VK_RECORDER_IDLE", "RX_VK_RECORDER_SPIN_US",
            "RX_VK_RECORDER_STATS", "RX_VK_RECORDER_RING_MB", "RX_VK_RECORDER_POISON")


def run(exe, case, env_add):
    env = {k: v for k, v in os.environ.items() if k not in SWITCHES}
    env["RX_VK_RECORDER"] = "thread"
    env["RX_VK_RECORDER_STATS"] = "0"
    env.update(env_add)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    try:
        r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=60,
                           creationflags=flags)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired as e:
        return None, (e.stdout or "") if isinstance(e.stdout, str) else "TIMEOUT"


OWN, IDLE = "RX_VK_RECORDER_OWN", "RX_VK_RECORDER_IDLE"
# (case, its switches, what the env case's line must say)
CASES = (
    ("env", {}, "mode 3 own 0 idle_wait 0"),
    ("env", {OWN: "1", IDLE: "wait"}, "mode 3 own 1 idle_wait 1"),
    ("env", {OWN: "0", IDLE: "spin"}, "mode 3 own 0 idle_wait 0"),
    ("env", {OWN: "off"}, "mode 3 own 0 idle_wait 0"),
    ("own_skip", {OWN: "1"}, None),
    ("own_off", {}, None),
    ("own_off", {OWN: "0"}, None),
    ("own_other", {OWN: "1"}, None),
    ("own_outside", {OWN: "1"}, None),
    ("own_guard", {OWN: "1"}, None),
    ("idle_default", {}, None),
    ("idle_no_hooks", {IDLE: "wait"}, None),
    ("idle_parked", {IDLE: "wait"}, None),
    ("idle_race", {IDLE: "wait"}, None),
)


def check(exe, cases=CASES):
    """the failures, as text"""
    bad = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda c: (c, run(exe, c[0], c[1])), cases))
    for (case, env, line), (code, out) in results:
        label = "%s %s" % (case, " ".join("%s=%s" % kv for kv in env.items()) or "(defaults)")
        if code != 0:
            bad.append("%s: exit %s\n%s" % (label, code, out.strip()[-600:]))
        elif line and line not in out:
            bad.append("%s: expected %r\n%s" % (label, line, out.strip()[-600:]))
        elif case == "env" and env.get(OWN) == "1" and ("RX_VK_RECORDER_OWN=1)" not in out or "RX_VK_RECORDER_IDLE=wait)" not in out):
            bad.append("%s: the recorder's log line does not name the switches\n%s" % (label, out.strip()[-600:]))
    return bad


# (each broken vk_recorder.h, and the case that must catch it)
BROKEN = (
    ("vkEndCommandBuffer among the slot's own calls", "return k == B_vkResetCommandPool ||",
     "return k == B_vkEndCommandBuffer || k == B_vkResetCommandPool ||", "own_other", {OWN: "1"}),
    ("the guard removed", "    if (g->idle_at(end_pos)) {", "    if (end_pos || true) {", "own_guard", {OWN: "1"}),
    ("the switch ignored", 'g_own = env_on("RX_VK_RECORDER_OWN");', "g_own = true;", "own_off", {}),
    ("OwnSlot ignoring slot_begin()", "OwnSlot() { t_own_slot = t_slot_ready; }", "OwnSlot() { t_own_slot = true; }", "own_guard", {OWN: "1"}),
    ("the compare taken from W", "uint64_t compare = r;", "uint64_t compare = W.load(std::memory_order_acquire);", "idle_race", {IDLE: "wait"}),
    ("the wake removed", "            g_wake_by_address(&W);\n", "", "idle_parked", {IDLE: "wait"}),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--headers", required=True, help="Vulkan-Headers' include folder")
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    test_sources()
    recorder = (VK / "vk_recorder.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="hookshot-recorder-own-") as folder:
        out = Path(folder)
        bad = check(build(cxx, args.headers, out, recorder, "real"))
        if bad:
            raise SystemExit("FAIL:\n" + "\n".join(bad))
        print("PASS the switches: %d cases" % len(CASES))
        for i, (what, old, new, case, env) in enumerate(BROKEN):
            if recorder.count(old) != 1:
                raise SystemExit("broken %r: the anchor %r must appear once in vk_recorder.h" % (what, old))
            exe = build(cxx, args.headers, out, recorder.replace(old, new), "broken%d" % i)
            if not check(exe, ((case, env, None),)):
                raise SystemExit("FAIL: vk_recorder.h with %s passed %s" % (what, case))
            print("PASS broken: %s fails %s" % (what, case))
    print("recorder own/idle: all passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
