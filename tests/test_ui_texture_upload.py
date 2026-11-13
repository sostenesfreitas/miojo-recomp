"""Run the custom ImGui texture uploader with coherent and noncoherent host caches."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--case", default="all")
    p.add_argument("--cache", choices=["all", "coherent", "noncoherent"], default="all")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_imgui.cpp").read_text(encoding="utf-8")
    draw = (a.source / "third_party/imgui/imgui_draw.cpp").read_text(encoding="utf-8")
    state = source[source.index("struct Tex {"):source.index("void reap(bool all)")]
    actual = "\n".join(block(source, marker) for marker in ["void pixels(", "void update_texture(", "void reap(bool all)"])
    fixture = (ROOT / "tests/ui_texture_upload_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* STATE */", state).replace("/* UPLOADER */", actual)
    fixture = fixture.replace("/* DESTROY_PIXELS */", block(draw, "void ImTextureData::DestroyPixels()"))
    cases = ["rgba-create", "alpha-create", "rgba-dirty", "alpha-dirty", "rgba-fallback", "alpha-fallback",
             "failure-buffer", "failure-image", "failure-view", "failure-descriptor", "destroy", "no-backend", "empty"]
    if a.case != "all":
        assert a.case in cases
        cases = [a.case]
    with tempfile.TemporaryDirectory(prefix="ui-texture-upload-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-I" + str(a.headers), "-I" + str(a.source / "third_party/imgui"),
                        str(cpp), "-o", str(exe)], check=True)
        for cache in (["coherent", "noncoherent"] if a.cache == "all" else [a.cache]):
            for case in cases:
                subprocess.run([str(exe), case, cache], check=True, timeout=10)


if __name__ == "__main__":
    main()
