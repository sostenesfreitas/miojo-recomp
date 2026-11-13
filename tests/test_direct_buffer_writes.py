"""Actual direct-buffer binding/write-page bookkeeping with authored pools; no Vulkan/game required."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]
CASES = ("mirror_off", "upload_off", "refused", "virtual_write", "large_write", "no_recording",
         "wait_disabled", "readonly", "mirror_pending", "mirror_on", "invalid_pool", "invalid_mapping")


def block(source, signature, semicolon=False):
    start = source.index(signature)
    first = source.index("{", start)
    depth, at = 1, first + 1
    while depth:
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
        at += 1
    if semicolon:
        assert source[at] == ";"
        at += 1
    return source[start:at]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"), help="GCC/Clang C++ compiler")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_memory.cpp",
                        help="Public vk_memory.cpp override for an original-source negative control")
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    source = args.source.read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")
    declarations = "\n".join(re.findall(r"^constexpr uint64_t [^;\n]+;", source, re.M))
    dimensions = []
    for name in ("kPageShift", "kChunkPages", "kPages"):
        value = re.search(r"\b" + name + r"\s*=\s*([^,;]+)[,;]", declarations)
        assert value, "Missing production page-table dimension: " + name
        dimensions.append("constexpr uint64_t %s = %s;" % (name, value[1]))
    types = "\n".join(dimensions) + "\n\n" + "\n\n".join([block(header, "struct BufferRef {", True), block(source, "struct Pool {", True),
                             block(source, "struct TlPool {", True), block(source, "struct WriteBack {", True)])
    pages = source[source.index("std::mutex wb_pages_mu;"):source.index("void wb_pages_note(")]
    direct_note = block(source, "void note_direct_write(") if "void note_direct_write(" in source else ""
    actual = "\n\n".join([pages, block(source, "void wb_pages_note("), block(source, "uint64_t memory_writeback_value("),
        block(source, "uint64_t gpu_bind_stamp("), block(source, "uint32_t bind_slot("), block(source, "bool bind_memo_hit("),
        block(source, "struct BindMemo {", True), "thread_local BindMemo bind_memo[256];", direct_note,
        block(source, "void shadow_forget(Pool& p, uint64_t at, uint64_t len) {"),
        block(source, "BufferRef gpu_buffer_bind(")])
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="hookshot-direct-writes-") as folder:
        tmp = Path(folder)
        (tmp / "direct_write_types.inc").write_text(types + "\n", encoding="utf-8")
        (tmp / "direct_write_actual.inc").write_text(actual + "\n", encoding="utf-8")
        exe = tmp / ("direct_writes.exe" if os.name == "nt" else "direct_writes")
        command = [cxx, "-std=c++20", "-include", ENV_ON_H, "-O0", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                   "-I" + str(tmp), str(ROOT / "tests/direct_buffer_writes_test.cpp"), "-o", str(exe)]
        result = subprocess.run(command, cwd=tmp, env=env, capture_output=True, text=True, timeout=60,
                                creationflags=flags)
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
        failed = 0
        for case in CASES:
            result = subprocess.run([str(exe), case], cwd=tmp, env=env, capture_output=True, text=True,
                                    timeout=10, creationflags=flags)
            print((result.stdout + result.stderr).strip())
            failed += result.returncode != 0
        print("direct buffer writes: %d cases, %d failed" % (len(CASES), failed))
        return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
