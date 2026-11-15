"""HOS_HLE_STATS=1 counts every HLE function's calls (host/hos.h's HLE macro), every host call and every call of the sdk's
NVN exports (host/nvn_native.c nvn2_dispatch), and logs every 5 s how often each ran (host/hos_hostcall.c), in the lines
scripts/cpu_report.py --calls reads. Off (the default) nothing is counted and host calls keep their direct path. The real
macro and the real host-call and dispatch code are compiled with stubs for the rest of the runtime; the report is read
with cpu_report.py's own parser, with another thread's line logged in between. Long overloads keep distinct counts.
Nine C++ broken variants and the old cross-window report formula must fail.

python -I -B tests/test_hle_stats.py [--cxx clang++]
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
# (env_on, the switches' rule, before the code cut out of the sources)
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h")
LONG_NAME = "_ZN2nn5audio42SetI3dl2ReverbLateReverbDecayHighFrequencyRatioEPNS0_15I3dl2ReverbTypeEf"
OVERLOAD_A = "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE"
OVERLOAD_B = "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_10ThreadTypeE"
MANY = 200   # (host calls called once each: more rows than any fixed cap)


def cut(source, start, end, what):
    if start not in source:
        raise SystemExit("FAIL: %s: the anchor %r is gone: update the test" % (what, start))
    i = source.index(start)
    if end not in source[i:]:
        raise SystemExit("FAIL: %s: the anchor %r is gone: update the test" % (what, end))
    return source[i:source.index(end, i)]


def hle_macro(header):
    """hos.h's counter type, switch and HLE macro (the macro's continued lines and the one after)"""
    part = cut(header, "typedef struct HleCount", "#define HLE(sym)", "host/hos.h")
    rest = header[header.index("#define HLE(sym)"):].split("\n")
    n = 0
    while rest[n].endswith("\\"):
        n += 1
    return part + "\n".join(rest[:n + 1]) + "\n"


HARNESS = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif
typedef int32_t LONG;   /* (as Windows' LONG and host/posix/windows.h's) */
typedef uint32_t ULONG;
typedef unsigned long DWORD;
typedef void *PVOID;
typedef void *HANDLE;
#define WINAPI
typedef struct { int x; } SRWLOCK;
#define SRWLOCK_INIT {0}
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
/* (wrapping, as the interlocked operations do) */
static LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static long InterlockedIncrement(volatile long *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static long InterlockedExchange(volatile long *p, long v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static void *InterlockedCompareExchangePointer(void *volatile *p, void *x, void *c) {
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c;
}
static DWORD GetCurrentThreadId(void) { return 1; }
typedef DWORD (*Start)(void *);
#ifdef _WIN32
extern "C" __declspec(dllimport) HANDLE CreateThread(void *, size_t, Start, void *, DWORD, DWORD *);
extern "C" __declspec(dllimport) DWORD WaitForSingleObject(HANDLE, DWORD);
extern "C" __declspec(dllimport) int CloseHandle(HANDLE);
extern "C" __declspec(dllimport) void Sleep(DWORD);
static const auto native_create = CreateThread;
static const auto native_close = CloseHandle;
static void yield() { Sleep(0); }
#else
static void yield() { sched_yield(); }
#endif
static Start started;
static int create_fails, closed;
static int64_t fake_ns = 1000000000;
static int64_t hos_now_ns() { return fake_ns; }
static void fake_sleep(DWORD ms) { (void)ms; }
static int fake_close(HANDLE h) { if (h) closed++; return 1; }
static void *fake_create(void *a, size_t s, Start fn, void *p, DWORD f, DWORD *id) {
    (void)a; (void)s; (void)p; (void)f; (void)id;
    if (create_fails) return NULL;
    started = fn; return (void *)1;
}
#define CreateThread fake_create
#define Sleep fake_sleep
#define CloseHandle fake_close
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    if (strstr(fmt, "calls of")) puts("vcores: another thread's line");   /* (logged between a report's head and rows) */
}
static void hos_fatal(void *c, const char *m) { (void)c; puts(m); exit(3); }
static int hos_self(void) { return 0; }
static const char *hos_thread_name(int t) { (void)t; return "main"; }
typedef struct { uint64_t x[31]; } Ctx;
typedef void (*A64Fn)(Ctx *c);
void hos_setjmp_return(Ctx *c, uint64_t target) { (void)c; (void)target; }
int hos_trace;
HLE_MACRO
HOSTCALL
uint64_t hos_hostcall(const char *name, HostFn fn, void *user);
static void submit(Ctx *c, void *user);
static uint64_t nvn_native_proc(const char *name) { return hos_hostcall(name, submit, NULL); }
DISPATCH
HLE(_ZN2nn2os9LockMutexEPNS0_9MutexTypeE) { c->x[0] += 1; }
HLE(_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE) { c->x[0] += 2; }
HLE(LONG_NAME) { c->x[0] += 4; }
HLE(OVERLOAD_A) { c->x[0] += 8; }
HLE(OVERLOAD_B) { c->x[0] += 16; }
static int submits;
static void submit(Ctx *c, void *user) { (void)c; (void)user; submits++; }
static void call_host(Ctx *c, uint64_t addr) {   /* (as lifted code does: the direct function a64_lookup gave, else a64_call_miss) */
    const A64Fn direct = a64_lookup_host(addr);
    if (direct) direct(c);
    else a64_call_miss(c, addr);
}
static volatile LONG ready, go;
static DWORD worker(void *) {
    Ctx c = {{0}};
    InterlockedIncrement(&ready);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) yield();
    for (int i = 0; i < 25000; i++) {
        imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
        imp_OVERLOAD_A(&c);
    }
    return 0;
}
static int concurrent() {
    hos_hle_stats_start();
#ifdef _WIN32
    HANDLE threads[4];
    for (auto &t : threads) { t = native_create(NULL, 0, worker, NULL, 0, NULL); if (!t) return 1; }
#else
    pthread_t threads[4];
    for (auto &t : threads) if (pthread_create(&t, NULL, [](void *p)->void * { worker(p); return NULL; }, NULL)) return 1;
#endif
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 4) yield();
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 16; i++) { fake_ns += 5500000000; hos_hle_report(); yield(); }
#ifdef _WIN32
    for (auto t : threads) { if (WaitForSingleObject(t, 10000) != 0) return 1; native_close(t); }
#else
    for (auto t : threads) pthread_join(t, NULL);
#endif
    fake_ns += 5500000000; hos_hle_report();
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "concurrent")) return concurrent();
    create_fails = argc > 1 && !strcmp(argv[1], "fail");
    const int on = argc > 1 && !create_fails;
    if (on || create_fails) hos_hle_stats_start();
    if (create_fails && (hos_hle_stats || started || closed)) { puts("FAIL: failed creation left counters on"); return 1; }
    if (on && closed != 1) { puts("FAIL: thread handle was not closed"); return 1; }
    if (on != (started != NULL)) { puts("FAIL: the 5 s thread started without HOS_HLE_STATS, or not with it"); return 1; }
    Ctx c = {{0}};
    for (int i = 0; i < 30; i++) imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
    for (int i = 0; i < 10; i++) imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
    imp_LONG_NAME(&c);
    for (int i = 0; i < 60; i++) imp_OVERLOAD_A(&c);
    for (int i = 0; i < 40; i++) imp_OVERLOAD_B(&c);
    if (c.x[0] != 30 + 20 + 4 + 60 * 8 + 40 * 16) { puts("FAIL: an HLE function's body did not run once a call"); return 1; }
    const uint64_t addr = hos_hostcall("nvnQueueSubmit", submit, NULL);
    printf("direct %d\n", a64_lookup_host(addr) != NULL);
    for (int i = 0; i < 7; i++) call_host(&c, addr);
    static void *k[2];   /* (gen/nvn2_exports.c: an sdk NVN export's cache) */
    for (int i = 0; i < 3; i++) nvn2_dispatch(&c, "nvnQueueSubmit", k);
    for (int i = 0; i < MANY; i++) {
        char name[16];
        snprintf(name, sizeof name, "h%03d", i);
        call_host(&c, hos_hostcall(name, submit, NULL));
    }
    if (submits != 10 + MANY) { puts("FAIL: a host call did not run"); return 1; }
    if (!on && __atomic_load_n(&hos_hle_list, __ATOMIC_ACQUIRE)) {
        puts("FAIL: HLE calls were counted while off"); return 1;
    }
    if (!on) for (int i = 0; i < ncalls; i++) if (__atomic_load_n(&calls[i].calls, __ATOMIC_RELAXED)) {
        puts("FAIL: host calls were counted while off"); return 1;
    }
    fake_ns = 6500000000; hos_hle_report();
    puts("---");
    imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
    calls[0].calls = calls[0].before = 0x7FFFFFFE;   /* (nvnQueueSubmit's count passes the 32-bit maximum) */
    for (int i = 0; i < 3; i++) call_host(&c, addr);
    fake_ns = 13000000000; hos_hle_report();
    return 0;
}
"""


def load_parser():
    sys.path.insert(0, str(ROOT / "scripts"))
    spec = importlib.util.spec_from_file_location("cpu_report", ROOT / "scripts" / "cpu_report.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse(hle_calls, folder, block):
    """the report as cpu_report.py reads it: (head numbers, {name: calls in the 5 s}, names in printed order)"""
    head = re.search(r"hle: (\d+) calls of (\d+) functions in ([0-9.]+) s", block)
    (folder / "game.log").write_text(block, encoding="utf-8")
    rates, secs = hle_calls(str(folder), blocks=1)
    order = [name.strip() for line in block.splitlines() if re.search(r"hle:( \d+ [^;]+;)+\s*$", line)
             for _, name in re.findall(r"(\d+) ([^;]+?)(?:;|$)", line.split("hle: ", 1)[1])]
    rows = {k: round(v * secs) for k, v in rates.items()} if rates else {}
    return head and (int(head.group(1)), int(head.group(2)), float(head.group(3))), rows, order


def check(hle_calls, folder, out, on):
    """what is wrong with one run's output, or None"""
    first, _, second = out.partition("---\n")
    direct = "direct 1" in first
    head, rows, order = parse(hle_calls, folder, first)
    if not on:
        if not direct:
            return "off: a host call lost its direct path"
        if head is not None or rows:
            return "off: something was counted: %r %r" % (head, rows)
        return None
    if direct:
        return "on: a host call kept its direct path (not counted)"
    want = {"_ZN2nn2os9LockMutexEPNS0_9MutexTypeE": 30, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE": 10,
            "nvnQueueSubmit": 10, LONG_NAME: 1, OVERLOAD_A: 60, OVERLOAD_B: 40}
    want.update(("h%03d" % i, 1) for i in range(MANY))
    if head != (151 + MANY, 6 + MANY, 5.5) or rows != want:
        missing = {k: (rows.get(k), v) for k, v in want.items() if rows.get(k) != v}
        return "on: the first report reads %r, %d rows; wrong or missing: %r" % (head, len(rows), dict(list(missing.items())[:6]))
    counts = [want[n] for n in order]
    if counts != sorted(counts, reverse=True):
        return "on: not the most called first: %r" % order[:6]
    head, rows, _ = parse(hle_calls, folder, second)
    if head != (4, 2, 6.5) or rows != {"_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE": 1, "nvnQueueSubmit": 3}:
        return "on: the second report is not the calls since the first (3 across the 32-bit wrap): %r %r" % (head, rows)
    return None


def run(compiler, folder, hle_calls, sources, tag):
    """None when both runs read right, else what went wrong"""
    header, hostcall, dispatch = sources
    src = (HARNESS.replace("HLE_MACRO", hle_macro(header)).replace("HOSTCALL", hostcall).replace("DISPATCH", dispatch)
           .replace("LONG_NAME", LONG_NAME).replace("OVERLOAD_A", OVERLOAD_A).replace("OVERLOAD_B", OVERLOAD_B)
           .replace("MANY", str(MANY)))
    (folder / (tag + ".cpp")).write_text(src, encoding="utf-8")
    exe = folder / (tag + (".exe" if os.name == "nt" else ""))
    subprocess.run([compiler, "-std=c++17", *([] if os.name == "nt" else ["-pthread"]), "-include", ENV_ON_H, "-O1", "-w", str(folder / (tag + ".cpp")), "-o",
                    str(exe)], check=True, timeout=120)
    for on in (False, True):
        result = subprocess.run([str(exe)] + (["on"] if on else []), capture_output=True, text=True, timeout=30)
        if result.returncode:
            return result.stdout.strip()[-300:] or "exit %d" % result.returncode
        wrong = check(hle_calls, folder, result.stdout, on)
        if wrong:
            return wrong
    failed = subprocess.run([str(exe), "fail"], capture_output=True, text=True, timeout=30)
    if failed.returncode or check(hle_calls, folder, failed.stdout, False):
        return "thread-creation failure did not keep the off path"
    if tag == "real":
        parallel = subprocess.run([str(exe), "concurrent"], capture_output=True, text=True, timeout=30)
        if parallel.returncode:
            return "concurrent callers/report failed"
        (folder / "game.log").write_text(parallel.stdout)
        entries = load_parser().hle_blocks(str(folder))
        counts = {}
        for entry in entries:
            for name, n in entry["calls"].items():
                counts[name] = counts.get(name, 0) + n
        if counts != {"_ZN2nn2os9LockMutexEPNS0_9MutexTypeE": 100000, OVERLOAD_A: 100000}:
            return "concurrent reports lost calls: %r" % counts
    return None


def check_windows(report, folder):
    origin = 1000000000
    name = OVERLOAD_A

    def block(begin, end, count=300):
        return ("hle: %d calls of 1 functions in %.9f s; begin_ns=%d end_ns=%d\n"
                "hle: %d %s;\n" % (count, end - begin, origin + begin * 1000000000,
                                    origin + end * 1000000000, count, name))

    log = "".join(block(t, t + 5, 5000 if t < 15 else 300) for t in range(0, 30, 5))
    (folder / "game.log").write_text(log + "hle: 100 calls of 2 functions in 5 s\nhle: 20 partial;\n")
    rates, seconds = report.hle_calls(str(folder), blocks=1)
    assert rates == {name: 60.0} and seconds == 5, "an incomplete final report was included"
    (folder / "load.json").write_text(json.dumps(dict(window_present_s=[12, 32])))
    times = sorted(set(range(36)) | {i + 0.5 for i in range(15, 30)})
    rows = [(origin, 0)]
    for t in times[1:]:
        end = origin + round(t * 1e9)
        rows.append((end, end - rows[-1][0]))

    def frames(values):
        (folder / "presents.csv").write_text("return_ns,interval_ns\n" +
                                           "".join("%d,%d\n" % row for row in values))

    frames(rows)
    for invalid in ([], {"counter_window": None}, {"counter_window": []}):
        (folder / "load.json").write_text(json.dumps(invalid))
        try:
            report.hle_measurement(str(folder))
        except ValueError as error:
            assert "must be an object" in str(error)
        else:
            raise AssertionError("malformed metadata was accepted")
    (folder / "load.json").write_text(json.dumps(dict(window_present_s=[12, 32])))
    measured = report.hle_measurement(str(folder))
    assert measured["calls"] == {name: 900} and measured["frames"] == 30 and measured["seconds"] == 15
    assert measured["span"] == (origin + 15000000000, origin + 30000000000), "loading entered the counter window"
    # (the QPC window is authoritative when present, without a second legacy window)
    (folder / "load.json").write_text(json.dumps(dict(counter_window=dict(analysis_window_ns=[origin + 12000000000,
                                                                                            origin + 32000000000]))))
    assert report.hle_measurement(str(folder)) == measured
    for change in (-100000000, 100000000):
        broken = list(rows)
        i = next(i for i, row in enumerate(broken) if row[0] == origin + 20000000000)
        broken[i] = broken[i][0], broken[i][1] + change
        frames(broken)
        try:
            report.hle_measurement(str(folder))
        except ValueError:
            pass
        else:
            raise AssertionError("a native frame gap/overlap was accepted")
    frames([row for row in rows if row[0] < origin + 30000000000])
    try:
        report.hle_measurement(str(folder))
    except ValueError:
        pass
    else:
        raise AssertionError("native frames did not cover the counter span")
    frames(rows)
    (folder / "game.log").write_text("hle: 1 calls of 1 functions in 5 s\nhle: 1 legacy;\n")
    try:
        report.hle_measurement(str(folder))
    except ValueError:
        pass
    else:
        raise AssertionError("legacy counters without clock boundaries were normalized")
    (folder / "game.log").write_text(log)
    stack = "# t0_ns %d\n" % origin + "".join(
        "%d MainThread|hookshot.exe+%s\n" % (ms, "2000" if 15000 <= ms <= 30000 else "2100")
        for ms in range(0, 35001, 500))
    (folder / "all.txt").write_text(stack)

    class Names:
        game_detail = False

        def of(self, frame):
            sym = name if frame.endswith("2000") else OVERLOAD_B
            return "imp_" + sym, "nn_os_sync.o", "hookshot.exe", "os/sync"

        def blocked(self, _):
            return False

    costs = report.hle_sampled_costs(str(folder), Names(), measured)
    assert set(costs) == {name} and abs(costs[name] - 15000000 / 900) < 1e-8, costs
    (folder / "all.txt").write_text(stack.replace("# t0_ns %d\n" % origin, ""))
    assert not report.hle_sampled_costs(str(folder), Names(), measured), "stacks with no clock origin were used"
    (folder / "all.txt").write_text("# t0_ns %d\n0 MainThread|\n20000 MainThread|hookshot.exe+2000\n"
                                   "35000 MainThread|\n" % origin)
    assert not report.hle_sampled_costs(str(folder), Names(), measured), "empty stacks supplied false clock coverage"
    (folder / "all.txt").write_text(stack)
    (folder / "load.json").write_text(json.dumps(dict(window_present_s=[12, 32])))
    (folder / "hookshot.map").write_text(
        "Preferred load address is 0000000140000000\n"
        " 0001:00001000 imp_%s 0000000140002000 f nn_os_sync.o\n"
        " 0001:00001100 imp_%s 0000000140002100 f nn_os_sync.o\n" % (name, OVERLOAD_B))
    args = sys.argv
    try:
        sys.argv = ["cpu_report.py", str(folder), "--maps", str(folder), "--calls", str(folder),
                    "--out", str(folder / "report.md")]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            report.main()
    finally:
        sys.argv = args
    text = (folder / "report.md").read_text()
    assert "estimated occupancy us/call" in text and "30 | 16666.67 |" in text, "CLI used another window's ms/frame"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    report = load_parser()
    hle_calls = report.hle_calls
    assert OVERLOAD_A[:64] == OVERLOAD_B[:64] and OVERLOAD_A != OVERLOAD_B
    for name in (LONG_NAME, OVERLOAD_A, OVERLOAD_B):
        if report.calls_key("imp_" + name) != name:
            raise SystemExit("FAIL: the report merges distinct full HLE function names")
    # (an HLE body the compiler did not inline is the symbol hle_<sym> in the linker map: the report names it as its
    # imp_<sym> entry, so its samples meet its call counts)
    with tempfile.TemporaryDirectory(prefix="hle-map-") as temporary:
        path = Path(temporary) / "hookshot.map"
        path.write_text("Preferred load address is 0000000140000000\n"
                        " 0001:00001000       hle__ZN2nn2os9LockMutexEPNS0_9MutexTypeE 0000000140002000 f   nn_os_sync.o\n"
                        " 0001:00001100       imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE 0000000140002100 f   nn_os_sync.o\n",
                        encoding="utf-8")
        names = [n for n, _ in report.load_map(str(path))[1]]
    if names != ["imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE"] * 2:
        raise SystemExit("FAIL: cpu_report.py names an HLE body not inlined %r, not as its imp_ entry" % names)
    header = (ROOT / "host" / "hos.h").read_text(encoding="utf-8")
    hostcall = cut((ROOT / "host" / "hos_hostcall.c").read_text(encoding="utf-8"), "#define HOS_HOSTCALL_BASE",
                   "/* Call guest code from inside a host function", "host/hos_hostcall.c")
    dispatch = cut((ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8"), "static void nvn2_counted(",
                   "/* nvnBootstrapLoader", "host/nvn_native.c")
    main_c = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    if not re.search(r'if \(env_on\("HOS_HLE_STATS"\)\) \{\s*void hos_hle_stats_start\(void\);\s*hos_hle_stats_start\(\);',
                     main_c):
        raise SystemExit("FAIL: host/hos_main.c no longer starts the counters on HOS_HLE_STATS (env_on)")
    if main_c.index('env_on("HOS_HLE_STATS")') > main_c.index("if (!reserve_guest())"):
        raise SystemExit("FAIL: host/hos_main.c starts the counters after the game's memory is set up")

    real = (header, hostcall, dispatch)
    for shared, order in (("h->listed", "__ATOMIC_ACQUIRE"), ("&hos_hle_list", "__ATOMIC_ACQUIRE"),
                          ("h->calls", "__ATOMIC_RELAXED"), ("calls[i].calls", "__ATOMIC_RELAXED")):
        if "__atomic_load_n(&%s, %s)" % (shared.lstrip("&"), order) not in hostcall:
            raise SystemExit("FAIL: non-atomic counter/publication read: " + shared)
    edits = (("truncated names merge HLE overloads", 1, 'hos_log("hle: %lu %s;",', 'hos_log("hle: %lu %.64s;",'),
             ("an HLE macro that never counts", 0, "if (hos_hle_stats) hos_hle_counted(", "if (0) hos_hle_counted("),
             ("an HLE macro that counts with the switch off", 0, "if (hos_hle_stats) hos_hle_counted(", "if (1) hos_hle_counted("),
             ("host calls kept direct with the switch on", 1, '|| hos_trace || hos_hle_stats || env_on("HOS_HOSTCALL_SLOW")',
              '|| hos_trace || env_on("HOS_HOSTCALL_SLOW")'),
             ("NVN exports not counted", 2, "&& hos_hle_stats) fn = nvn2_counted", "&& 0) fn = nvn2_counted"),
             ("a report capped at 150 rows", 1, "for (int i = 0; i < n; i++) {", "for (int i = 0; i < n && i < 150; i++) {"),
             ("a count compared signed (skipped across the wrap)", 1,
              "const unsigned long d = (ULONG)now - (ULONG)calls[i].before;",
              "const unsigned long d = now > calls[i].before ? (ULONG)now - (ULONG)calls[i].before : 0;"),
             ("a fabricated five-second duration", 1, "(double)(end_ns - begin_ns) / 1e9", "5.0"),
             ("counting after a failed thread creation", 1,
              'if (!thread) { hos_hle_stats = 0; hos_log("hle: call counters could not start; counting stays off"); return; }',
              'if (!thread) hos_log("hle: failed");'))
    with tempfile.TemporaryDirectory(prefix="hle-stats-") as temporary:
        folder = Path(temporary)
        wrong = run(compiler, folder, hle_calls, real, "real")
        if wrong:
            raise SystemExit("FAIL: " + wrong)
        for i, (what, which, old, new) in enumerate(edits):
            if old not in real[which]:
                raise SystemExit("FAIL: the broken variant '%s' lost its anchor %r: update the test" % (what, old))
            broken = list(real)
            broken[which] = broken[which].replace(old, new, 1)
            if not run(compiler, folder, hle_calls, broken, "broken%d" % i):
                raise SystemExit("FAIL: the check passed " + what)
        check_windows(report, folder)
        # (the CLI must use the counter run's same-window stacks, not its other profile rows' ms/frame)
        text = Path(report.__file__).read_text(encoding="utf-8")
        old, new = "fmt(call_costs.get(calls_key(sym)), 2)", "fmt(msf * 1000.0 / cpf, 2) if cpf and msf else '-'"
        assert text.count(old) == 1
        bad = types.ModuleType("wrong_call_window")
        bad.__file__ = report.__file__
        exec(compile(text.replace(old, new), report.__file__, "exec"), bad.__dict__)
        try:
            check_windows(bad, folder)
        except AssertionError as error:
            assert "CLI used another window" in str(error), error
        else:
            raise SystemExit("FAIL: the CLI check accepted the old cross-window per-call formula")
    print("PASS: HOS_HLE_STATS off counts nothing and keeps host calls direct; on, every HLE function, host call and NVN "
          "export call is counted, and each timed report, read by cpu_report.py with another line in between, has every "
          "function's calls since the last (across the 32-bit wrap), most called first, full names kept distinct; "
          "creation failure keeps counting off; concurrent callers/report, complete blocks, matched native/sample "
          "windows and the CLI pass; %d broken variants caught" % len(edits))


if __name__ == "__main__":
    main()
