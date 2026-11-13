"""The storage buffer tables searched in a loop on Intel (mma_layout.h global_table_loop).

Mesa inlines every function, so each global access of OoT's culling programs carried its ~70 table entries written out: Intel's
compiler did not finish 53caac5f in minutes, and every compute build queued behind it waited. This checks the switch (Intel's,
dynamic indexing required, RX_VK_GM_LOOP over it), the program cache's stamp, and that the loop tests what the written-out form
tests, entry by entry: the same words of the same constant buffer, the same bounds, the first match taken.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
ENV_H = ROOT / "gpu/vk/compat/hookshot_env.h"   # (env_value_off)
DRIVER_IDS = {"VK_DRIVER_ID_MESA_RADV": 3, "VK_DRIVER_ID_NVIDIA_PROPRIETARY": 4,   # (as the Vulkan headers number them)
              "VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS": 5, "VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA": 6}


def block(text, marker, start=0):
    start = text.index(marker, start)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def switch(cxx):
    """table_loop_wanted cut from vk_coopmat.cpp and run for the GPUs and settings that matter"""
    coopmat = (ROOT / "gpu/vk/vk_coopmat.cpp").read_text(encoding="utf-8")
    fn = block(coopmat, "bool table_loop_wanted(")
    init = block(coopmat, "void coopmat_init(")
    call = re.search(r"global_table_loop = table_loop_wanted\(([^;]+)\);", init)
    if not call:
        raise SystemExit("coopmat_init does not set global_table_loop from table_loop_wanted")
    args = " ".join(call.group(1).split())
    for needed in ("dev.driver_id", "shaderStorageBufferArrayDynamicIndexing", '"RX_VK_GM_LOOP"'):
        if needed not in args:
            raise SystemExit(f"table_loop_wanted is not given {needed}")
    mesa, windows = "VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA", "VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS"
    nvidia, radv = "VK_DRIVER_ID_NVIDIA_PROPRIETARY", "VK_DRIVER_ID_MESA_RADV"
    cases = [  # driver, dynamic indexing, RX_VK_GM_LOOP, wanted
        (mesa, True, None, True, "Intel, Mesa"),
        (mesa, False, None, False, "Intel, Mesa, without dynamic indexing"),
        (mesa, True, "0", False, "Intel, Mesa, RX_VK_GM_LOOP=0"),
        (windows, True, None, False, "Intel, Windows driver"),
        (nvidia, True, None, False, "NVIDIA"),
        (radv, True, None, False, "AMD, RADV"),
        (radv, True, "1", True, "AMD, RADV, RX_VK_GM_LOOP=1"),
        (radv, True, "true", True, "AMD, RADV, RX_VK_GM_LOOP=true"),
        (nvidia, False, "1", False, "NVIDIA without dynamic indexing, RX_VK_GM_LOOP=1"),
    ]
    checks = "\n".join(
        f'    if (table_loop_wanted({v}, {str(d).lower()}, {"nullptr" if e is None else chr(34) + e + chr(34)}) != '
        f'{str(w).lower()}) {{ std::puts("FAIL: {name}"); return 1; }}'
        for v, d, e, w, name in cases)
    driver_enum = "enum VkDriverId { " + ", ".join(f"{k} = {n}" for k, n in DRIVER_IDS.items()) + " };\n"
    source = (ENV_H.read_text(encoding="utf-8") + "\n#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n" + driver_enum + fn +
              "\nint main() {\n" + checks + "\n    return 0;\n}\n")
    with tempfile.TemporaryDirectory(prefix="table-loop-") as folder:
        folder = Path(folder)
        (folder / "t.cpp").write_text(source, encoding="utf-8")
        exe = folder / "t.exe"
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", str(folder / "t.cpp"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        if run.returncode:
            raise SystemExit(run.stdout.strip() or "the switch check failed")
    return len(cases)


def stamp():
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    body = block(shader, "uint64_t translator_stamp(")   # (the program cache's stamp: vk_progcache.cpp's disk_stamp takes it)
    if not re.search(r"global_table_loop \? \d+u : 0u", body):
        raise SystemExit("translator_stamp does not take global_table_loop: programs cached in one form are used in the other")


def emission():
    emit = (ROOT / "gpu/vk/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp").read_text(encoding="utf-8")
    body = block(emit, "const auto define_body{")
    head = body[:body.index("const size_t num_buffers")]
    # (the counter is a function variable: it must be made at the start of the function's first block, which only define_body's
    # own AddLabel opens; the CAS callers open the block themselves and keep the written-out form)
    if "AddLocalVariable" not in head or "add_label" not in head[head.index("table_counter"):]:
        raise SystemExit("the loop's counter is not made in the function's first block, or not only where define_body opens it")
    tables = body[body.index("for (const GlobalTableVars& table : ssbo_tables)"):]
    loop_at = tables.index("if (Sirit::ValidId(table_counter))")
    loop = block(tables, "if (Sirit::ValidId(table_counter))")
    written = block(tables, "for (u32 k = 0; k < table.count; ++k)", loop_at + len(loop))
    for needed, why in (
        ("OpStore(table_counter, zero);", "the counter starts at 0 for each table"),
        ("spv::LoopControlMask::DontUnroll", "the driver is asked not to unroll the loop back into the written-out form"),
        ("OpULessThan(U1, k, Const(table.count))", "the loop runs over the table's entries"),
        ("OpIAdd(U32[1], k, Const(1U))", "the counter steps by one entry"),
        ("continue;", "the written-out form is skipped where the loop was emitted"),
    ):
        if needed not in loop:
            raise SystemExit(f"the table loop: {why} ({needed} not found)")
    # entry k is the 16 bytes at cbuf_offset + 16k: its address the U32x2 word (cbuf_offset + 16k) / 8, its size the next word
    if "Const(at / 8)" not in written or "Const(at / 8 + 1)" not in written or "table.cbuf_offset + k * 16" not in written:
        raise SystemExit("the written-out form's addressing changed: update this check and the loop with it")
    word = re.search(r"const Id word\{OpIAdd\(U32\[1\], Const\(table\.cbuf_offset / 8\), "
                     r"OpShiftLeftLogical\(U32\[1\], k, Const\(1U\)\)\)\};", loop)
    if not word or "OpIAdd(U32[1], word, Const(1U))" not in loop:
        raise SystemExit("the loop does not read entry k's words as the written-out form does (cbuf_offset / 8 + 2k, and the next)")
    for offset in range(0, 4096, 4):
        for k in range(16):
            if (offset + k * 16) // 8 != offset // 8 + 2 * k:
                raise SystemExit(f"entry {k} at offset {offset}: the loop's word is not the written-out form's")
    # the same test in both: the masked address, the end from the size word capped by what is bound, the binding's end without a size
    for needed in ("OpBitwiseAnd(U64, unaligned_addr, Constant(U64, ssbo_align_mask))",
                   "ssbo_end = OpSelect(U64, OpIEqual(U1, size_word, zero), bound_end, capped);",
                   "OpLogicalAnd(U1, OpUGreaterThanEqual(U1, addr, ssbo_addr), OpULessThan(U1, addr, ssbo_end))",
                   "OpShiftRightLogical(U32[1], ssbo_offset, Const(shift))"):
        if needed not in loop or needed not in written:
            raise SystemExit(f"the loop and the written-out form differ: {needed}")
    if "callback(OpAccessChain(element_pointer, table_id, k, zero, ssbo_index));" not in loop:
        raise SystemExit("the loop does not hand entry k's element to the access")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    n = switch(args.cxx)
    stamp()
    emission()
    print(f"PASS: table loop switch ({n} cases), cache stamp, and the loop tests what the written-out form tests")


if __name__ == "__main__":
    main()
