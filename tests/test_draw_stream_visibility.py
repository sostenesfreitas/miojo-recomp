"""Execute production stream allocation/reset and serial/segment submission against host caches."""
import argparse
from pathlib import Path
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--cache", choices=["all", "coherent", "noncoherent"], default="all")
    p.add_argument("--case", default="all")
    a = p.parse_args()
    draw = (a.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/draw_stream_visibility_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* RES */", block(draw, "struct RecordingRes {") + ";")
    actual = "\n".join(block(draw, marker) for marker in ["bool stream_insert(", "bool stream_next(", "bool stream_reserve(", "uint64_t stream_alloc("])
    actual += "\n" + block(draw, "BufferRef stream_copy(")
    fixture = fixture.replace("/* STREAM */", actual)
    flush = block(draw, "void draw_flush_recording(") if "void draw_flush_recording(" in draw else ""
    fixture = fixture.replace("/* FLUSH */", flush).replace("/* BEGIN */", block(draw, "void draw_begin_recording("))
    fixture = fixture.replace("/* SUBMIT */", block(replay, "void submit_recording("))
    start = replay.index("        if (j->inits < 0) init_lost = true;")
    end = replay.index("        if (res == VK_ERROR_DEVICE_LOST) device_lost();", start)
    end += len("        if (res == VK_ERROR_DEVICE_LOST) device_lost();")
    fixture = fixture.replace("/* SEGMENT */", replay[start:end])
    cases = ["single", "grow", "reuse", "empty", "batch", "segments", "failed-segment", "allocation-failure"]
    if a.case != "all":
        assert a.case in cases
        cases = [a.case]
    with tempfile.TemporaryDirectory(prefix="draw-stream-visibility-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-pthread", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-I" + str(a.headers), str(cpp), "-o", str(exe)], check=True)
        for cache in (["coherent", "noncoherent"] if a.cache == "all" else [a.cache]):
            for case in cases:
                subprocess.run([str(exe), case, cache], check=True, timeout=20)


if __name__ == "__main__":
    main()
