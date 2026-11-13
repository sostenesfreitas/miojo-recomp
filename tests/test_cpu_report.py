"""scripts/cpu_report.py on a synthetic capture: linker maps in lld's format, an all-thread sample file and load.json, no game.
Checks the owner and self views (a driver leaf under the renderer is charged to the renderer in the owner view), the guest
subsystem map, blocked samples left out of CPU time, the per-thread table, unclassified symbols listed, and the categories'
own rule order; with a broken variant: a rule set where nothing matches must leave everything unclassified.

    python -I -B tests/test_cpu_report.py
"""
import importlib, json, os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "cpu_report.py"

EXE_MAP = """ hookshot

 Preferred load address is 0000000140000000

 Start         Length     Name                   Class
 0001:00000000 00100000H .text                   CODE
 0001:00000000       f_80001000                 0000000140001000     code_hot_1.o
 0001:00001000       f_80900000                 0000000140002000     code_010.o
 0001:00002000       imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE 0000000140003000     host_nn_os_sync.o
 0001:00003000       a64_lookup                 0000000140004000     a64_rt.o
 0001:00004000       brand_new_function         0000000140005000     host_brand_new_file.o
 0001:00005000       mystery                    0000000140006000     zz_unknown.o
"""
VK_MAP = """ hookshot_vk

 Preferred load address is 0000000180000000

 Start         Length     Name                   Class
 0001:00000000 00100000H .text                   CODE
 0001:00000000       draw_one                   0000000180001000     vk_vk_draw.o
 0001:00001000       take_capture               0000000180002000     vk_vk_memory.o
"""
# (one line a thread an instant: "<ms> <thread>|<leaf>;...;<root>", module+offset; offsets are relative to the image base)
SAMPLES = [
    "0 sead::MainThread|hookshot.exe+1010;hookshot.exe+2020",         # f_80001000 (game: core), under f_80900000
    "0 Presentation_Thread|amdvlk64.dll+500;hookshot_vk.dll+1010",    # driver leaf, owner draw_one (render)
    "2 sead::MainThread|hookshot.exe+3010;hookshot.exe+1010",         # LockMutex (os/sync), called from game
    "2 Presentation_Thread|hookshot_vk.dll+2010",                     # take_capture (render/capture)
    "4 sead::MainThread|hookshot.exe+1010",                           # game again
    "4 Presentation_Thread|hookshot_vk.dll+1010",                     # draw_one
    "4 BaseProcExecutor3|hookshot.exe+6010;hookshot.exe+2020",        # zz_unknown.o: unclassified
    "6 sead::MainThread|hookshot.exe+4010;hookshot.exe+1010",         # a64_lookup: runtime/indirect-calls
    "6 Presentation_Thread|ntdll.dll+1010;hookshot_vk.dll+1010",      # blocked in a wait: not CPU time
]
NTDLL_MAP = """ ntdll

 Preferred load address is 0000000180000000

 Start         Length     Name                   Class
 0001:00000000       NtWaitForSingleObject      0000000180001000     ntdll.o
"""
CANDIDATES = {"candidates": [{"id": "t", "title": "Test draw", "match": "^draw_one$", "what": "w", "fix": "f", "gain": [0.5, 0.5],
                              "risk": "r", "evidence": "e"}], "game_own": []}


def run(tmp, cats_text=None, extra=(), candidates=None, guest=True, samples=None):
    maps, rundir = tmp / "maps", tmp / "run1"
    maps.mkdir(exist_ok=True)
    rundir.mkdir(exist_ok=True)
    (maps / "hookshot.map").write_text(EXE_MAP, encoding="utf-8")
    (maps / "hookshot_vk.map").write_text(VK_MAP, encoding="utf-8")
    (maps / "ntdll.dll.map").write_text(NTDLL_MAP, encoding="utf-8")
    (rundir / "all.txt").write_text("\n".join(samples or SAMPLES) + "\n", encoding="utf-8")
    scripts = tmp / "scripts"
    scripts.mkdir(exist_ok=True)
    (scripts / "cpu_report.py").write_text(SCRIPT.read_text(encoding="utf-8"), encoding="utf-8")
    text = cats_text if cats_text is not None else (ROOT / "scripts" / "cpu_categories.py").read_text(encoding="utf-8")
    (scripts / "cpu_categories.py").write_text(text, encoding="utf-8")
    # (a local guest map, as a developer would pass it; without it lifted code must be one row)
    (tmp / "guest_map.py").write_text("GUEST_MAP = [(0x80000000, 0x80800000, 'main', 'core', 'test'), "
                                      "(0x80800000, 0x81000000, 'main', 'actors', 'test')]\n", encoding="utf-8")
    if guest:
        extra = ("--guest-map", str(tmp / "guest_map.py")) + tuple(extra)
    (scripts / "cpu_functions.json").write_text(json.dumps({"draw_one": {"does": "translates one draw"}}), encoding="utf-8")
    (scripts / "cpu_candidates.json").write_text(json.dumps(candidates or CANDIDATES), encoding="utf-8")
    out = tmp / "report.md"
    r = subprocess.run([sys.executable, "-I", "-B", str(scripts / "cpu_report.py"), str(rundir), "--maps", str(maps), "--out", str(out),
                        "--fps", "50", "--csv", str(tmp / "csv"), *extra], capture_output=True, text=True, timeout=120)
    if r.returncode:
        raise SystemExit("cpu_report.py failed:\n" + r.stdout + r.stderr)
    return out.read_text(encoding="utf-8")


def row(md, label):
    for line in md.splitlines():
        if line.startswith("| ") and label in line.split("|")[1]:
            return line
    return None


def main():
    failures = []
    with tempfile.TemporaryDirectory(prefix="cpu-report-") as t:
        md = run(Path(t))
        # 4 instants; on-CPU samples: game 2 (core) + LockMutex 1 + runtime 1 + render 3 + unclassified 1
        expect = {"game/main/core": "50.0", "os/sync": "25.0", "runtime/indirect-calls": "25.0", "render/draw-translation": "50.0",
                  "render/capture": "25.0", "unclassified": "25.0"}
        owner = md.split("## By category: who executes it")[0]
        for cat, pct in expect.items():
            r = row(owner, cat)
            if not r or (" %s " % pct) not in r and ("**%s**" % pct) not in r:
                failures.append("owner view: %s should be %s%% of a core: %s" % (cat, pct, r))
        selfv = md.split("## By category: who executes it")[1].split("## Threads")[0]
        if not row(selfv, "**driver**"):
            failures.append("self view: the driver leaf should be its own category")
        if row(owner, "**driver**"):
            failures.append("owner view: the driver leaf should be charged to the renderer, not to driver")
        if "translates one draw" not in md:
            failures.append("the function description was not merged")
        if "zz_unknown.o" not in md.split("## Unclassified")[-1]:
            failures.append("the unclassified symbol was not listed")
        th = row(md.split("## Threads")[1], "Presentation_Thread")
        if not th or "| 75.0 | 15.00 |" not in th:   # (75 % of a core at 50 fps is 15 ms a frame)
            failures.append("thread table: the Presentation_Thread should be on the CPU 75%% of the time: %s" % th)
        # (ms a frame = % of a core x 10 / fps: render 75 % at 50 fps is 15 ms, draw translation 50 % is 10 ms)
        if "**15.00**" not in (row(owner, "**render**") or "") or "| 10.00 |" not in (row(owner, "render/draw-translation") or ""):
            failures.append("ms a frame should follow --fps: %s / %s" % (row(owner, "**render**"), row(owner, "render/draw-translation")))
        # (the blocked sample is not CPU time, and is listed where the thread waits)
        blocked = [l for l in md.split("## Blocked")[-1].splitlines() if l.startswith("| Presentation_Thread")]
        if not blocked or "`draw_one`" not in blocked[0]:
            failures.append("the blocked sample should be listed under Blocked, under the function that waits: %s" % blocked)
        # (a candidate is priced from the samples with its function in the stack: draw_one in 2 of 4 instants; at 50 fps
        # 50 % of a core is 10 ms a frame, the gain half of it; control: a match nothing has costs nothing)
        def cand_row(text):
            return next((l for l in text.split("## What could be")[-1].splitlines() if "**Test draw**" in l), None)
        cand = cand_row(md)
        if not cand or "| 50.0 (10.00) |" not in cand or "| 5.00 to 5.00 |" not in cand:
            failures.append("candidate pricing: %s" % cand)
        none = dict(CANDIDATES, candidates=[dict(CANDIDATES["candidates"][0], match="^nothing$")])
        cand0 = cand_row(run(Path(t), candidates=none))
        if not cand0 or "| 0.0 (0.00) |" not in cand0:
            failures.append("CONTROL: a candidate matching nothing should cost nothing: %s" % cand0)
        # (broken variant: rules that match nothing must leave everything unclassified)
        cats = (ROOT / "scripts" / "cpu_categories.py").read_text(encoding="utf-8")
        broken = cats.replace("RULES = [", "RULES = [] and [", 1)
        md2 = run(Path(t), broken)
        if not row(md2.split("## By category: who executes it")[0], "**unclassified**") or row(md2, "os/sync"):
            failures.append("CONTROL: with no rules, everything should be unclassified")
        # (what may be published: no guest map, so no guest address and no game subsystem; lifted code is one row)
        pub = run(Path(t), guest=False, samples=SAMPLES + ["6 Thread_8000abcd|hookshot.exe+1010"])   # (an unnamed thread)
        if re.search(r"Thread_[0-9a-fA-F]{8}", pub) or "unnamed thread" not in pub:
            failures.append("without --guest-map an unnamed thread's guest entry address must not be printed")
# (a Linux capture names hookshot / hookshot_vk.so, whose maps are not read: refused, even with Windows maps present)
        try:
            run(Path(t), samples=["0 sead::MainThread|hookshot+1010;hookshot+2020"])
            failures.append("a Linux capture should be refused, not mislabelled")
        except SystemExit as e:
            if "Linux capture" not in str(e):
                failures.append("a Linux capture failed for another reason: %s" % e)
        if re.search(r"f_[0-9a-fA-F]{8}", pub) or "game/main" in pub or not row(pub, "lifted game code"):
            failures.append("without --guest-map the report must print no guest address or subsystem, and one lifted row")
        if not row(pub.split("## By category: who executes it")[0], "game/lifted"):
            failures.append("without --guest-map lifted code should be the game/lifted category")
    sys.path.insert(0, str(ROOT / "scripts"))
    rep = importlib.import_module("cpu_report")
    with tempfile.TemporaryDirectory(prefix="cpu-calls-") as t:   # (HOS_HLE_STATS summaries: the last blocks only)
        (Path(t) / "game.log").write_text(
            "[hos host] hle: 900 calls of 2 functions in 5 s\n[hos host] hle: 800 _ZN2nn2os9LockMutexEPNS0_9MutexTypeE; 100 nvnQueueSubmitCommands\n"
            "[hos host] hle: 60 calls of 2 functions in 5 s\n[hos host] hle: 50 _ZN2nn2os9LockMutexEPNS0_9MutexTypeE; 10 nvnQueueSubmitCommands\n"
            "[hos host] vk: something else\n", encoding="utf-8")
        cps, secs = rep.hle_calls(t, blocks=1)
        if secs != 5.0 or cps.get("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE") != 10.0 or cps.get("nvnQueueSubmitCommands") != 2.0:
            failures.append("hle_calls should read the last block only: %r %r" % (secs, cps))
    with tempfile.TemporaryDirectory(prefix="cpu-gpu-") as t:   # (two GPUs: the report names the one the renderer chose)
        (Path(t) / "game.log").write_text(
            "[hos host] vk: device 0: Integrated GPU, Vulkan 1.3, driver 0x1, type 1\n"
            "[hos host] vk: device 1: Discrete GPU, Vulkan 1.4, driver 0x2, type 2\n"
            "[hos host] vk: Discrete GPU, Vulkan 1.4, driver 0x2; mesh shaders yes, host memory import yes, pipeline libraries yes\n",
            encoding="utf-8")
        gpu = rep.machine(t).get("gpu", "")
        if not gpu.startswith("Discrete GPU"):
            failures.append("machine() should name the chosen GPU, not device 0: %r" % gpu)
    if rep.calls_key("imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE") != "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE" or \
            rep.calls_key("n_nvnQueueSubmitCommands") != "nvnQueueSubmitCommands":
        failures.append("calls_key does not map frame keys to the counter's names")
    # (descriptions lose sentences that state a measurement, and keep behaviour that only sounds like one)
    for text, measured in (("About 740 a frame; the cost is the wake.", True), ("0.8 % of on-CPU.", True),
                           ("A third of all on-CPU samples are under it.", True), ("It runs 7,600 times a second.", True),
                           ("Havok navigation: a second avoidance kernel.", False), ("Finds a draw that samples a target.", False),
                           ("Spins for up to 20 us (HOS_SPIN_US) on a flag.", False),
                           ("Three of four waits end inside the spin.", True), ("What is left is two or three cache misses a lookup.", True),
                           ("A larger cache was measured worse.", True), ("Waits for one of the six cores.", False),
                           ("Selects one of two buffers and waits for its fence.", False),
                           ("Workers wait hundreds of times a frame.", True),
                           ("The game creates over a hundred command buffers a frame.", True),
                           ("Marks one bit for a few hundred pages.", False)):
        if bool(rep.MEASURE.search(text)) != measured:
            failures.append("measurement filter: %r should %sbe dropped" % (text, "" if measured else "not "))
    if rep.nn_name("imp__ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE") != "nn::os::WaitLightEvent":
        failures.append("nn_name does not demangle the HLE entry")
    c = importlib.import_module("cpu_categories")
    for r in c.RULES:
        if len(r) != 4 or not isinstance(r[3], str):
            failures.append("a rule is not (module, object, symbol, category): %r" % (r,))
    if failures:
        raise SystemExit("\n".join(failures))
    print("PASS cpu_report: owner and self views, guest subsystems, threads, descriptions, unclassified; control caught")


if __name__ == "__main__":
    main()
