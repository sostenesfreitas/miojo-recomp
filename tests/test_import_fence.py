"""A host-memory import's region ends with its range while the driver is asked (gpu/vk/import_fence.h).

On Windows the fixture runs the fence on real pages: VirtualQuery from the range's start reports the whole run of
read-write pages without it and the range alone with it, the page gets its protection back, and a reserved or
read-only page after the range is left as it is. Elsewhere it checks that the header builds and does nothing.
No Vulkan or game data.
python -I -B tests/test_import_fence.py [--cxx g++]
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="g++")
    args = ap.parse_args()
    # (the import itself: the fence stands around the driver's call, and not beside the CPU write watches)
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    body = source[source.index("bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {"):]
    body = body[:body.index("\n}\n")]
    block = re.search(r"\{\s*const ImportFence fence\(ptr, bytes, ([^;]*)\);\s*allocated = vkAllocateMemory\([^;]*\);\s*\}", body)
    if not block:
        raise SystemExit("FAIL: import_pool does not hold an ImportFence over its vkAllocateMemory")
    if "!page_watches" not in block.group(1):
        raise SystemExit("FAIL: the fence is not left out with the CPU write watches")
    with tempfile.TemporaryDirectory(prefix="import-fence-") as tmp:
        exe = Path(tmp) / "import_fence_test.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/import_fence_test.cpp"), "-o", str(exe)], check=True, timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(result.stdout.strip(), result.stderr.strip())
        if result.returncode:
            raise SystemExit("FAIL: import_fence_test")
    print("PASS: an import's region ends with its range while the fence stands, and the page is as it was after")


if __name__ == "__main__":
    main()
