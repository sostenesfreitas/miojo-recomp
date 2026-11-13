"""Compile the opt-in Vulkan hardware fixture without loading Vulkan or executing GPU work."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", type=Path)
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_xfer.cpp")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx) or (args.cxx if Path(args.cxx).is_file() else None)
    if not compiler:
        print(f"SKIP: C++ compiler unavailable: {args.cxx}")
        return 0
    if not args.headers or not (args.headers / "vulkan/vulkan_core.h").is_file():
        print("SKIP: Vulkan headers unavailable")
        return 0
    source = args.source.read_text(encoding="utf-8")
    production = block(source, "struct Sub {") + ";\n" + "\n".join(
        block(source, marker) for marker in (
            "VkImageSubresourceLayers layers(", "VkOffset3D offset(",
            "VkExtent3D extent(", "bool buffer_side("
        )
    )
    fixture = (ROOT / "tests/vulkan_copy_span_gpu.cpp").read_text(encoding="utf-8").replace(
        "// PRODUCTION_COPY_LAYOUT", production
    )
    with tempfile.TemporaryDirectory(prefix="hookshot-copy-span-gpu-compile-") as directory:
        cpp, exe = Path(directory) / "fixture.cpp", Path(directory) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        flags = ["-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers"]
        if os.name == "nt" and "mingw" in compiler.lower():
            flags.append("-static")
        libraries = ["-ldl"] if os.name != "nt" else []
        subprocess.run([compiler, *flags, "-I" + str(args.headers), str(cpp), "-o", str(exe), *libraries],
                       check=True, timeout=120)
    print("PASS: Vulkan hardware fixture compiled; no Vulkan loader or GPU work executed")


if __name__ == "__main__":
    main()