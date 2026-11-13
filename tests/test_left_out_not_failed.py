"""The networks a card leaves out on purpose (gpu/vk/vk_shader.cpp translate_into: the movie upscaler's networks on a card that
is not NVIDIA, with RX_VK_NO_F16, or without shaderFloat16) are not shader failures: a linear upscale takes their place. They
were counted in st_failed and logged as "not translated", and used up the 30 lines that name real failures (issue 2943), so a
program that really failed after a preparation on such a card was never named, and every "prepared" and statistics line counted
the networks as failed.

translate_into's catch is cut from the source and run with a recording logf: 40 networks left out, then a real failure. Then
three more networks with RX_VK_IR set, and one under a seed answer's trial. Expected: one line for the networks, the real
failure's line, failed 1, left out 43 (the trial's not counted), and with RX_VK_IR each network's key. No game or GPU required.

python -I -B tests/test_left_out_not_failed.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#define _CRT_SECURE_NO_WARNINGS 1
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
static std::vector<std::string> lines;
static void logf(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    lines.push_back(b);
}
std::atomic<int64_t> st_failed, st_left_out;
struct Forced { std::string want; };
static Forced* forced;
struct Program { uint64_t key; bool failed; };
static void set_ir(const char* v) {   // (RX_VK_IR on, nullptr: off)
#ifdef _WIN32
    _putenv_s("RX_VK_IR", v ? v : "");
#else
    v ? setenv("RX_VK_IR", v, 1) : unsetenv("RX_VK_IR");
#endif
}
/* LEFT_OUT */

static void translate(Program& p, bool network) {
    try {
        if (network) /* THROW */
        throw std::runtime_error("SPIR-V emit failed");
/* CATCH */
}

static size_t count(const char* s) {
    size_t n = 0;
    for (const auto& l : lines) n += l.find(s) != std::string::npos;
    return n;
}

int main() {
    int fails = 0;
    for (uint64_t k = 1; k <= 40; k++) {
        Program p{0x1000 + k, false};
        translate(p, true);
        if (!p.failed) std::printf("FAIL: a left-out network's program is not marked failed (nothing to draw it with)\n"), fails++;
    }
    Program real{0xbad, false};
    translate(real, false);
    if (!real.failed) std::printf("FAIL: the real failure is not marked failed\n"), fails++;
    if (count("0000000000000bad not translated: SPIR-V emit failed") != 1)
        std::printf("FAIL: the real failure after 40 left-out networks is not logged\n"), fails++;
    if (count("not translated") != 1) std::printf("FAIL: %zu lines say not translated, 1 expected\n", count("not translated")), fails++;
    if (st_failed != 1) std::printf("FAIL: failed %lld, 1 expected\n", (long long)st_failed.load()), fails++;
    if (count("left out") != 1) std::printf("FAIL: %zu lines for the left-out networks, 1 expected\n", count("left out")), fails++;
    set_ir("1");
    for (uint64_t k = 41; k <= 43; k++) {
        Program p{0x1000 + k, false};
        translate(p, true);
    }
    set_ir(nullptr);
    if (count("000000000000102b") != 1 || count("left out") != 4)
        std::printf("FAIL: with RX_VK_IR each left-out network is not named\n"), fails++;
    Forced trial{"x"};
    forced = &trial;
    Program t{0x2000, false};
    translate(t, true);
    forced = nullptr;
    if (st_left_out != 43 || st_failed != 1)
        std::printf("FAIL: left out %lld, failed %lld; 43 and 1 expected\n", (long long)st_left_out.load(), (long long)st_failed.load()), fails++;
    for (const auto& l : lines) std::printf("  %s\n", l.c_str());
    return fails ? 1 : 0;
}
'''


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    body = src[src.index("void translate_into("):]
    catch = body[body.index("    } catch ("):body.index("    st_translate_ns +=")]
    at = body.index('("a network left out on this card");')
    throw = body[body.rindex("throw ", 0, at):body.index(";", at) + 1]
    left_out = block(src, "struct LeftOut") + ";" if "struct LeftOut" in src else ""
    code = DRIVER.replace("/* LEFT_OUT */", left_out).replace("/* THROW */", throw).replace("/* CATCH */", catch)
    with tempfile.TemporaryDirectory(prefix="left-out-") as tmp:
        cpp, exe = Path(tmp) / "left_out.cpp", Path(tmp) / "left_out.exe"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(cpp), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: the networks a card leaves out are counted or logged as failures:\n" + r.stdout + r.stderr)
    print("PASS: the networks a card leaves out are one line and their own count; a real failure after them is logged and counted")


if __name__ == "__main__":
    main()
