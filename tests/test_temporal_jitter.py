"""Compile the actual temporal-jitter publishers, readers, replay callback and host transport; no GPU, SDK or game."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    dlss = (a.source / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    fsr = (a.source / "gpu/vk/vk_fsr3_inputs.cpp").read_text(encoding="utf-8")
    replay = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    host = (a.source / "host/rx_backend.c").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/temporal_jitter_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* HOST_CACHE_LOCKED */", "#define HOST_CACHE_LOCKED " +
                              str(int("AcquireSRWLockExclusive" in block(host, "int hos_dlss_jitter_clip("))))
    if "MkwVkJitter temporal_jitter_sample()" in dlss:
        reader = block(dlss, "MkwVkJitter temporal_jitter_sample()") + "\n" + block(dlss, "uint32_t temporal_jitter_size(int axis)")
        producer = re.search(r"s\.jitter\.publish\([^;]+;", dlss).group()
        fr = block(fsr, "MkwVkJitter jitter_sample()") + "\n" + block(fsr, "uint32_t jitter_size(int axis)")
        callback = block(replay, "void jitter_sample(MkwVkJitter* out)")
        # The actual public table producer must install the callback the host consumes.
        assert "t.jitter_sample = jitter_sample;" in block(replay, "void fill_replay_table(")
    else:
        reader = block(dlss, "uint32_t temporal_jitter_size(int axis)")
        reader += "\nMkwVkJitter temporal_jitter_sample() { return {temporal_jitter_size(0),temporal_jitter_size(1),temporal_jitter_size(2)}; }"
        producer = re.search(r"s\.jitter_w = rw, s\.jitter_h = rh, s\.jitter_phases = [^;]+;", dlss).group()
        fr = block(fsr, "uint32_t jitter_size(int axis)")
        fr += "\nMkwVkJitter jitter_sample() { return {jitter_size(0),jitter_size(1),jitter_size(2)}; }"
        callback = "void jitter_sample(MkwVkJitter* out) { *out=temporal_jitter_sample(); if(!out->width)*out=fsr3_inputs::jitter_sample(); }"
    for tag, text in [("/* DLSS_READER */", reader), ("/* DLSS_PUBLISH */", producer), ("/* FSR_READER */", fr),
                      ("/* FSR_PUBLISH */", block(fsr, "void upscaled(")), ("/* REPLAY_CALLBACK */", callback),
                      ("/* HOST_TRANSPORT */", block(host, "int hos_dlss_jitter_clip("))]:
        fixture = fixture.replace(tag, text)
    with tempfile.TemporaryDirectory(prefix="temporal-jitter-") as folder:
        root = Path(folder)
        cpp, exe = root / "fixture.cpp", root / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        includes = ["-I" + str(a.source), "-I" + str(ROOT)]
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-pthread", *includes, str(cpp), "-o", str(exe)], check=True)
        for args in ([], ["off"], ["fsr-jitter-off"], ["parallel-host"], ["late-host"]):
            subprocess.run([str(exe), *args], check=True, timeout=30)


if __name__ == "__main__":
    main()
