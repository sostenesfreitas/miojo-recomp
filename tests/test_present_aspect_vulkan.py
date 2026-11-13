"""Optional real-Vulkan offscreen presentation regression, without game data.

Needs a Vulkan 1.3 device, loader and headers. The source suite skips it; CI's
separate lavapipe job and explicit selections run it. No window/swapchain is created.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
# (a GPU)
RUN_ALL_SKIP = {
    "linux": "needs a Vulkan 1.3 device for offscreen transfer/readback",
    "windows": "needs a Vulkan 1.3 device for offscreen transfer/readback",
}

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--headers", required=True, type=Path)
    parser.add_argument("--device", help="case-insensitive device-name substring; no fallback to another device")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    begin = source.index("    const PresentAspect aspect{", source.index("void present_image("))
    end = source.index("    if (src && framelog_on())", begin)
    fixture = (ROOT / "tests/present_aspect_vulkan.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// @PRODUCTION_HELPERS@", function(source, "void image_barrier(") + "\n" +
                              function(source, "void blit_crop_to_rect("))
    fixture = fixture.replace("// @PRODUCTION_PRESENT@", source[begin:end])
    loader = [str(Path(os.environ.get("WINDIR", "C:/Windows")) / "System32/vulkan-1.dll")] if sys.platform == "win32" else ["-lvulkan"]
    with tempfile.TemporaryDirectory(prefix="present-aspect-vulkan-") as tmp:
        unit, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        unit.write_text(fixture, encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-missing-field-initializers", "-Wno-missing-braces",
                   "-I" + str(ROOT / "gpu/vk"), "-I" + str(args.headers), str(unit), *loader, "-o", str(exe)]
        subprocess.run(command, check=True, timeout=60)
        env = dict(os.environ)
        compiler = shutil.which(args.cxx)
        if compiler:
            env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)] + (["--device", args.device] if args.device is not None else []),
                       env=env, check=True, timeout=60)


if __name__ == "__main__":
    main()
