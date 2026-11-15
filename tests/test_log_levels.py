# SPDX-License-Identifier: GPL-3.0-or-later
"""A log line's level: none written for info, "[trace] " (and error, warn, debug) after the thread's tag for the rest.

The launcher's build log shows info and above unless asked for more, and reads a line's level from that tag, so an info line
must stay exactly as it was (the log files, the bug report and the scripts that read them) and the others must carry it in
the one place it looks. Compiled from the real source:

- host/hos_thread.c hos_vlog and log_level_tag, with host/hos.h's levels: the line each level writes, the tag after
  "[hos <thread>] " (where the launcher reads it), an info line as it was.
- gpu/vk/vk_main.cpp vlogf: the renderer's logf_at gives the host's log the same tag at the start of its text, and logf none.
- gpu/vk/vk_shader.cpp: a movie network left out on a card that can't run it is a trace line, counted apart from the 30
  lines a program that failed to translate gets (on AMD the dozen networks took them all).

python -I -B tests/test_log_levels.py --cxx clang++
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(text, marker):
    """The function or block that starts at marker, through its closing brace"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def statement(text, marker):
    """The declaration that starts at marker, through its semicolon"""
    start = text.index(marker)
    return text[start:text.index(";", start) + 1]


CODE = r"""
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "hos_report_prefixes.h"
%(hos_enum)s
struct HosThread { const char* name; };
HosThread* cur;
volatile int hos_control_on;
void hos_control_log(const char*) {}
std::string queued;
static int log_queue(const char* line) { queued = line; return 1; }
std::string fatal_reason;
int fatal_calls;
static void hos_crash_record_fatal(const uint64_t*, const char* reason) {
    fatal_reason = reason;
    fatal_calls++;
}
%(tag)s
%(hos_vlog)s
void host_line(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hos_vlog(level, fmt, ap);
    va_end(ap);
}
%(enum)s
int64_t steady_ms() { return 0; }
std::atomic<int64_t> last_log_ms{0};
std::string got;
struct Host { void (*log)(const char*); };
const Host the_host{[](const char* line) { got = line; }};
const Host* host = &the_host;
%(vlogf)s
%(logf)s
%(logf_at)s
int fails = 0;
void want(const std::string& a, const char* b, const char* what) {
    if (a != b) { std::printf("FAIL: %%s: \"%%s\", not \"%%s\"\n", what, a.c_str(), b); fails++; }
}
int main() {
    want(log_level_tag(HOS_LOG_INFO), "", "host: info has no tag");
    want(log_level_tag(HOS_LOG_ERROR), "[error] ", "host: error");
    want(log_level_tag(HOS_LOG_WARN), "[warn] ", "host: warn");
    want(log_level_tag(HOS_LOG_DEBUG), "[debug] ", "host: debug");
    want(log_level_tag(HOS_LOG_TRACE), "[trace] ", "host: trace");
    host_line(HOS_LOG_INFO, "vk: device %%d", 1);
    want(queued, "[hos host] vk: device 1\n", "host: an info line as it was");
    host_line(HOS_LOG_TRACE, "vk: program %%s", "x");
    want(queued, "[hos host] [trace] vk: program x\n", "host: trace after the thread's tag");
    HosThread main_thread{"Main"};
    cur = &main_thread;
    host_line(HOS_LOG_DEBUG, "codec: y");
    want(queued, "[hos Main] [debug] codec: y\n", "host: debug, on a named thread");
    cur = nullptr;
    want(std::to_string(fatal_calls), "0", "host: normal lines do not ask for a crash record");
    host_line(HOS_LOG_WARN, "vk: device lost: reading the checkpoints");
    want(fatal_reason, "device lost", "host: classify raw message before its level tag");
    want(queued, "[hos host] [warn] vk: device lost: reading the checkpoints\n", "host: keep the warning tag");
    host_line(HOS_LOG_ERROR, "vk: FATAL: stopped");
    want(fatal_reason, "Vulkan fatal", "host: tagged fatal reaches the recorder");
    want(queued, "[hos host] [error] vk: FATAL: stopped\n", "host: keep the error tag");
    want(std::to_string(fatal_calls), "2", "host: one collector call per fatal message");
    logf("vk: device %%d", 1);
    want(got, "vk: device 1", "renderer: logf is info, untagged");
    logf_at(log_trace, "vk: program %%016llx not translated", 0x95fca6f01e3030aull);
    want(got, "[trace] vk: program 095fca6f01e3030a not translated", "renderer: trace");
    logf_at(log_debug, "x");
    want(got, "[debug] x", "renderer: debug");
    logf_at(log_info, "y");
    want(got, "y", "renderer: logf_at info is untagged");
    logf_at((LogLevel)42, "z");
    want(got, "z", "renderer: a level out of range is info");
    if (!fails) std::printf("PASS: the log's levels\n");
    return fails != 0;
}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    a = ap.parse_args()
    thread = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
    vk_main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    internal = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    hos_h = (ROOT / "host/hos.h").read_text(encoding="utf-8")
    code = CODE % {"hos_enum": statement(hos_h, "enum { HOS_LOG_ERROR"),
                   "tag": block(thread, "static const char *log_level_tag(int level) {"),
                   "hos_vlog": block(thread, "static void hos_vlog(int level, const char *fmt, va_list ap) {"),
                   "enum": statement(internal, "enum LogLevel"),
                   "vlogf": block(vk_main, "static void vlogf("),
                   "logf": block(vk_main, "void logf(const char* fmt, ...) {"),
                   "logf_at": block(vk_main, "void logf_at(LogLevel level, const char* fmt, ...) {")}
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "levels.cpp", Path(tmp) / ("levels" + (".exe" if os.name == "nt" else ""))
        src.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-format-security",
                        "-D_CRT_SECURE_NO_WARNINGS", "-I", str(ROOT / "host"), str(src), "-o", str(exe)], check=True, timeout=180)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        sys.stdout.write(r.stdout)
        if r.returncode:
            raise SystemExit("FAIL: the log's levels")
    # (the shader's catch: the network left out is told apart from a failure, at trace, on a count of its own)
    catch = shader[shader.index("static std::atomic<int> logged, left_out;"):]
    catch = catch[:catch.index("st_translate_ns")]
    if "strcmp(e.what(), network_left_out)" not in catch or "!trial && left_out++ < 30) logf_at(log_trace" not in catch:
        raise SystemExit("FAIL: a network left out is not a trace line counted apart")
    # (Hookshot throws it as LeftOut, which a catch of its own counts apart (st_left_out) and says once; its text is the same
    # as network_left_out, so the generic catch's comparison still tells it apart where it lands there)
    if 'throw LeftOut("a network left out on this card");' not in shader or "catch (const LeftOut& e)" not in shader or \
            'network_left_out[] = "a network left out on this card"' not in shader:
        raise SystemExit("FAIL: the network left out is not thrown with the text the catch compares")
    print("PASS: a network left out is a trace line, counted apart from failures")


if __name__ == "__main__":
    main()
