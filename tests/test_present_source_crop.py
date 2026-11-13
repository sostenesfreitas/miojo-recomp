"""Execute production presentation/capture blits and interpolation recording with CPU API spies."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--source", type=Path, default=ROOT)
    a = p.parse_args()
    replay = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    fg = (a.source / "gpu/vk/vk_framegen.cpp").read_text(encoding="utf-8")
    structs = fg[fg.index("struct Img {"):fg.index("int64_t now_ns()")]
    functions = "\n".join([block(replay, "void blit_crop_to_rect("), block(replay, "void blit_crop("),
                           block(fg, "int fixed_frames("), block(fg, "bool framegen_record("),
                           block(fg, "VkImage framegen_hdr(")])   # (fixed_frames: the frames a game frame gets, framegen_record asks it)
    template = (ROOT / "tests/present_source_crop_test.cpp").read_text(encoding="utf-8")
    fixture = template.replace("/* ACTUAL_STATE */", structs).replace("/* ACTUAL_FUNCTIONS */", functions)
    with tempfile.TemporaryDirectory(prefix="present-source-crop-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-pthread", "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(a.headers), "-I" + str(a.source / "gpu/vk"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
