"""The readback refresh in a parallel replay's submission keeps what the GPU wrote earlier in it.

A parallel replay (RX_VK_PARALLEL) records one submission as several recordings: the part before a big command buffer,
its segments and the part after, with one write-back at the end. memory_rb_refresh copies readback pages (OoT's exposure
meter) from guest memory into the mirror; in a later recording of the submission that copy put the guest's page back
over what the recordings before it had written (the meter's sum lost, every frame too bright), including writes the
write-back does not see (a bare table entry's guessed range past its first 64 bytes). Serial replay's copies must stay
what they were.

Cuts the production code (memory_rb_refresh and its bookkeeping, gpu_buffer_bind's write-back note, the texture
recording hooks and vk_draw.cpp's storage-descriptor binding) into tests/parallel_rb_refresh_test.cpp and runs three
cases: parallel (the meter's sum), untracked (a bare entry's write past its head), serial (the copies, exactly).

python -I -B tests/test_parallel_rb_refresh.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ("parallel", "untracked", "serial")


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def cut(source, start, end):
    if source.count(start) != 1:
        raise SystemExit("marker not found once: " + start)
    a = source.index(start)
    return source[a:source.index(end, a)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    memory = (vk / "vk_memory.cpp").read_text(encoding="utf-8")
    texture = (vk / "vk_texture.cpp").read_text(encoding="utf-8")
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    internal = (vk / "vk_internal.h").read_text(encoding="utf-8")

    # (the layout the fixture models: the part before, the segments and the part after share one submission)
    par = cut(replay, "memory_sync_mirrors_before_segments();   // (in r's uploads", "Recording& nr = begin_recording(rec_cur_value);")
    for step in ("submit_recording(r, false);", "texture_begin_recording(j->r->upload, j->r->value);", "j->uploads = texture_uploads_recorded();"):
        if step not in par:
            raise SystemExit("the parallel block no longer has: " + step)
    if "memory_begin_submission" in par or "memory_end_submission" in par:
        raise SystemExit("the parallel block's recordings no longer share one submission: update this test")
    segments_begin = "memory_rb_segments_begin(base, seg.size());" in par

    inc = {
        "rb_bufferref.inc": block(internal, "struct BufferRef {") + ";\n",
        "rb_pool.inc": block(memory, "struct Pool {") + ";\n",
        "rb_writeback.inc": block(memory, "struct WriteBack {") + ";\n",
        "rb_refresh.inc": cut(memory, "// ---- (from PR 430, YunaraDebugs) Readback pages", "BufferRef gpu_buffer(uint64_t gpu, uint64_t size) {"),
        "rb_bindtail.inc": cut(block(memory, "BufferRef gpu_buffer_bind("),
                               "if (p->synced_epoch != mirror_epoch || p->sync_seg > texture_segment()) mirror_sync(*p);",
                               "return remember({p->mirror"),
        "rb_texture.inc": "\n".join(block(texture, m) for m in (
            "uint64_t texture_segment() {", "void texture_set_segment(uint64_t seg) {",
            "void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {", "bool texture_uploads_recorded() {")),
        "rb_draw.inc": cut(draw, "if (d.count > 1 && !size && addr && !(addr & 3) && host->gpu_guest(addr))",
                           "// (Fritchii's storage buffer pass, Info::native_global_bounds)")
        + cut(draw, "const uint64_t base = addr & ~(salign - 1);", "{   // (EvilOOT) the wanted programs' storage descriptors"),
    }
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
    failed = []
    with tempfile.TemporaryDirectory(prefix="parallel-rb-refresh-") as tmp:
        folder = Path(tmp)
        for name, text in inc.items():
            (folder / name).write_text(text + "\n", encoding="utf-8")
        binary = folder / "fixture.exe"
        fixture = ROOT / "tests" / "parallel_rb_refresh_test.cpp"
        define = "HAVE_SEGMENTS_BEGIN=%d" % segments_begin
        if msvc:
            command = [compiler, "/nologo", "/std:c++20", "/FI" + ENV_ON_H, "/EHsc", "/D" + define, "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(folder),
                       "/Fo" + str(folder / "fixture.obj"), "/Fe" + str(binary), str(fixture)]
        else:
            command = [compiler, "-std=c++20", "-include", ENV_ON_H, "-O1", "-D" + define, "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(folder),
                       str(fixture), "-o", str(binary)] + ([] if os.name == "nt" else ["-pthread"])
        r = subprocess.run(command, capture_output=True, text=True, timeout=180)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        for case in CASES:
            r = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
            print(r.stdout, end="")
            print(r.stderr, end="", file=sys.stderr)
            if r.returncode:
                failed.append(case)
    if failed:
        raise SystemExit("FAIL: the readback refresh lost GPU writes or changed serial copies: " + ", ".join(failed))
    print("PASS: a parallel replay's refresh keeps the submission's GPU writes; serial copies are unchanged")


if __name__ == "__main__":
    main()
