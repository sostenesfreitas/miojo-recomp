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
     makes the wait return at once on the changed W;
   - own_slots: two slots with their own command pools, command buffers, descriptor pools, extra pools and sets; slot 1's binds held
     in the recorder while slot 0 comes round, its GPU work completing late: slot 0's own calls reach the driver while slot 1's
     packets are held, and the fakes, which know which object each held packet names, find no reset, begin or destruction of an
     object in use and no slot reused before its GPU work completed. _wrong (slot 1's pool reset) and _early (slot 0 reused before
     its GPU work completed) show the fakes catch each;
   - real_ (Windows; skipped elsewhere): vk_replay.cpp's own WaitOnAddress adapter (rec_wait_hooks, cut from the source): a publish
     wakes the recorder asleep on W; with no wake the 2 ms timeout brings it back; a publish just before the wait with the wake off;
     stop while asleep; both switches together. Each ends with vkDestroyDevice's teardown (the recorder stopped and joined) and a
     normal return.
3. The test has teeth: vk_recorder.h with vkEndCommandBuffer among the slot's own calls, the guard removed, the switch ignored,
   OwnSlot ignoring slot_begin(), the wait's compare taken from W instead of the r it last saw, the wake removed, or (Windows) the
   wait's timeout taken out, must fail it; so must (Windows) vk_replay.cpp's adapter with a wake that does nothing or a wait that
   compares against W as it is now (the real_parked and real_race waits get a 10 s timeout, so within the 3 s a case allows
   only the wake or the compare ends them), and the source check with recording_wait commented out or another slot's
   descriptor pools.
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


def code(text):
    """the text without its comments (// and /* */): a call commented out is not a call"""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def source_problems(replay, draw, others):
    """what is wrong with OwnSlot's placement; others: {name: text} of the renderer's other files"""
    p = []
    begin, submit, draw_begin = (code(body(replay, BEGIN, "vk_replay.cpp")), code(body(replay, SUBMIT, "vk_replay.cpp")),
                                 code(body(draw, DRAW_BEGIN, "vk_draw.cpp")))
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
    if not (0 <= begin.find("recording_wait(r);") < begin.find("rec::slot_begin(")):
        p.append("begin_recording: recording_wait(r) (the slot's GPU work done) must come before rec::slot_begin()")
    for call in ("rec::slot_begin(rec_end_pos[r.slot]);", "draw_begin_recording(r.slot, r.cmd, value);"):
        if begin.count(call) != 1:
            p.append("begin_recording: %s must appear once (the slot's own end position and descriptor pools)" % call)
    if not (0 <= draw_begin.find("RecordingRes& r = res[slot];") < draw_begin.find("rec::OwnSlot")):
        p.append("draw_begin_recording: the OwnSlot's calls must be on res[slot], the slot's own")
    dblock = draw_begin[draw_begin.find("rec::OwnSlot"):]
    dblock = dblock[:dblock.find("}")]
    for call in ("vkResetDescriptorPool(dev.device, r.pool, 0);", "for (VkDescriptorPool p : r.extra) vkDestroyDescriptorPool(dev.device, p,"):
        if call not in dblock:
            p.append("draw_begin_recording: %s not within the OwnSlot" % call)
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
    for what, old, new in (("recording_wait commented out", "recording_wait(r);", "// recording_wait(r);"),
                           ("recording_wait in a block comment", "recording_wait(r);", "/* recording_wait(r); */"),
                           ("another slot's descriptor pools", "draw_begin_recording(r.slot, r.cmd, value);",
                            "draw_begin_recording((r.slot + 1) % kRecordings, r.cmd, value);")):
        if begin.count(old) != 1 or not source_problems(replay.replace(begin, begin.replace(old, new, 1), 1), draw, others):
            raise SystemExit("sources: the control with %s passed" % what)
    print("PASS sources: OwnSlot only at the slot's own resets and begins, after slot_begin(); the end position kept after both ends")


HOOKS = "const bool rec_wait_hooks = [] {\n"


def wait_hooks():
    """the body of vk_replay.cpp's WaitOnAddress installation (rec_wait_hooks), for the real_ cases"""
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    if replay.count(HOOKS) != 1:
        raise SystemExit("vk_replay.cpp: the anchor %r must appear once" % HOOKS.strip())
    at = replay.index(HOOKS) + len(HOOKS)
    end = replay.index("\n}();", at)
    return replay[at:end] + "\n"


def build(cxx, headers, out, recorder_text, name, hooks_text=None):
    d = out / name
    d.mkdir()
    (d / "vk_loader.h").write_text(recsync.test_loader(), encoding="utf-8")
    (d / "vk_work.h").write_text((VK / "vk_work.h").read_text(encoding="utf-8"), encoding="utf-8")
    (d / "vk_recorder.h").write_text(recorder_text, encoding="utf-8")
    # (whatever else the two headers include from gpu/vk: a header added there later comes along, as #60's gpu_wait_stats.h)
    work = (VK / "vk_work.h").read_text(encoding="utf-8")
    for h in sorted(set(re.findall(r'#include "([^"/]+)"', work + recorder_text))):
        if h not in ("vk_loader.h", "vk_work.h", "vk_recorder.h") and (VK / h).exists():
            shutil.copyfile(VK / h, d / h)
    (d / "rec_wait_hooks.inc").write_text(hooks_text or wait_hooks(), encoding="utf-8")
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
    ("own_slots", {OWN: "1"}, None),
    ("own_slots_wrong", {OWN: "1"}, "caught: a descriptor pool reset while a held packet binds a set from it (slot 1)"),
    ("own_slots_early", {OWN: "1"}, "caught: slot reused before its GPU work completed (slot 0)"),
    ("real_parked", {IDLE: "wait"}, None),
    ("real_timeout", {IDLE: "wait"}, None),
    ("real_race", {IDLE: "wait"}, None),
    ("real_stop", {IDLE: "wait"}, None),
    ("real_both", {OWN: "1", IDLE: "wait"}, None),
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
    ("the wait's timeout taken out", "g_wait_on_address(&W, &compare, sizeof compare, 2);",
     "g_wait_on_address(&W, &compare, sizeof compare, 0xFFFFFFFFu);", "real_timeout", {IDLE: "wait"}),
)


# (vk_replay.cpp's WaitOnAddress adapter broken, and the real_ case that must catch it)
HOOKS_BROKEN = (
    ("a wake that does nothing", "[](void* a) { wake(a); }", "[](void*) {}", "real_parked"),
    ("a wait comparing against W as it is now", "{ return wait(a, c, n, ms) != 0; }",
     "{ (void)c; uint64_t now = *(volatile uint64_t*)a; return wait(a, &now, n, ms) != 0; }", "real_race"),
)


def caught(exe, case, env):
    """whether a broken build fails `case`. A real_ case runs the operating system's wait, which may return early (WaitOnAddress
    allows it) or start late under load, and so can now and then let a broken adapter through; a real break fails nearly every
    run. So a real_ case counts as caught when any of up to three runs fails: a fluke cannot make the suite intermittent"""
    for _ in range(3 if case.startswith("real_") else 1):
        if check(exe, ((case, env, None),)):
            return True
    return False


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
            if case.startswith("real_") and os.name != "nt":
                print("SKIP broken: %s (the real adapter is Windows')" % what)
                continue
            if recorder.count(old) != 1:
                raise SystemExit("broken %r: the anchor %r must appear once in vk_recorder.h" % (what, old))
            exe = build(cxx, args.headers, out, recorder.replace(old, new), "broken%d" % i)
            if not caught(exe, case, env):
                raise SystemExit("FAIL: vk_recorder.h with %s passed %s" % (what, case))
            print("PASS broken: %s fails %s" % (what, case))
        if os.name == "nt":   # (vk_replay.cpp's adapter broken: the real_ cases, which run without the timeout, must catch it)
            hooks = wait_hooks()
            for i, (what, old, new, case) in enumerate(HOOKS_BROKEN):
                if hooks.count(old) != 1:
                    raise SystemExit("broken adapter %r: the anchor %r must appear once in rec_wait_hooks" % (what, old))
                exe = build(cxx, args.headers, out, recorder, "hooks%d" % i, hooks.replace(old, new))
                if not caught(exe, case, {IDLE: "wait"}):
                    raise SystemExit("FAIL: the adapter with %s passed %s" % (what, case))
                print("PASS broken adapter: %s fails %s" % (what, case))
    print("recorder own/idle: all passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
