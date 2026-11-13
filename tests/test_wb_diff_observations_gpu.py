"""Compile the authored write-back shader fixture; --run executes it on an actual Vulkan device.

Normal CI compiles only. Running it needs Vulkan1.3, buffer device address and compute subgroup votes;
no game data, window or swapchain is used. --shader-header permits a causal run with the baseline shader.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--shader-header", type=Path, default=ROOT / "gpu/vk/shaders/wb_diff_comp_spv.h")
    p.add_argument("--run", action="store_true")
    p.add_argument("--device", default="")
    a = p.parse_args()
    header = a.shader_header.resolve()
    assert header.is_file()
    fixture = (ROOT / "tests/wb_diff_observations_gpu.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// @SHADER_HEADER@", '#include "' + header.as_posix() + '"')
    with tempfile.TemporaryDirectory(prefix="wb-observations-") as tmp:
        source, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        command = [a.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-missing-field-initializers", "-I" + str(a.headers), str(source), "-o", str(exe)]
        if sys.platform != "win32":
            command.append("-ldl")
        subprocess.run(command, check=True, timeout=90)
        if a.run:
            env = dict(os.environ)
            compiler = shutil.which(a.cxx)
            if compiler:
                env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            subprocess.run([str(exe), a.device], env=env, check=True, timeout=40)
        else:
            print("PASS: Vulkan write-back observation fixture compiles (GPU execution not requested)")


if __name__ == "__main__":
    main()
