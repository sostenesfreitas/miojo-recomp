"""Native FSR image ownership under delayed submission completion, without a GPU or game."""
RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

import argparse
import os
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--headers", required=True)
    args = parser.parse_args()
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    shared = block(replay, "uint64_t vk_retirement_value()") + "\n" + block(replay, "uint64_t vk_timeline_completed()") + "\n" + "\n".join(block(replay, m) for m in ["void queue_flush_locked()", "void submit_recording(", "void finish_recording(uint64_t value) {"])
    fixture = (ROOT / "tests/fsr_image_retirement_test.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="fsr-retirement-") as tmp:
        for backend in ["vk_fsr3.cpp", "vk_fsr3fi.cpp"]:
            source = (ROOT / "gpu/vk" / backend).read_text(encoding="utf-8")
            image = block(source, "struct Image {") + ";"
            state = "std::vector<std::pair<uint64_t,Image>> dead;" if backend == "vk_fsr3fi.cpp" else "struct {std::vector<std::pair<uint64_t,Image>> dead;} g;"
            actual = "\n".join(block(source, marker) for marker in ["void destroy_now(", "void drop_image(", "void free_dead("])
            code = fixture.replace("// PRODUCTION", shared + "\n" + image + "\n" + state + "\n" + actual)
            cpp = Path(tmp) / (backend + ".cpp")
            exe = Path(tmp) / (backend + (".exe" if os.name == "nt" else ".test"))
            cpp.write_text(code, encoding="utf-8")
            subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-I" + args.headers, str(cpp), "-o", str(exe)], check=True)
            for case in ["pending", "resumed", "post-present", "batch", "completed", "several", "out-of-order", "empty", "no-timeline", "overflow", "queue-serial", "queue-batch", "queue-parallel", "queue-post-present"]:
                subprocess.run([str(exe), case], check=True)
                print(backend, case, "passed", flush=True)


if __name__ == "__main__":
    main()
