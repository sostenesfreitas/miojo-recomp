"""Execute complete host/DMA import factories, rollback, fallback and real failure holds."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ['--cxx', '{cxx}', '--headers', '{vulkan_headers}']

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    owner_release = block(source, "void destroy_import_buffer(") if "void destroy_import_buffer(" in source else ""
    actual = owner_release + "\n" + "\n".join(block(source, marker) for marker in [
        "bool import_on_hold(", "void import_remember_failure(", "bool import_dma_buf(", "bool import_pool("])
    # Select both platform routes independently of the machine running the game-free test.
    assert actual.count("#ifdef __linux__") == 1
    actual = actual.replace("#ifdef __linux__", "#if TEST_DMA_FALLBACK")
    fixture = (ROOT / "tests/import_buffer_binding_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* ACTUAL */", actual)
    cases = [(route, case) for route in ["host", "dma"] for case in [
        "healthy", "create-error", "allocate-error", "host-oom", "device-oom", "unknown-bind"]]
    cases += [("host", "hold-retry"), ("host", "properties-error"), ("dma", "sparse-delegation"), ("dma", "nvidia")]
    with tempfile.TemporaryDirectory(prefix="import-buffer-binding-") as temp:
        cpp = Path(temp) / "fixture.cpp"
        cpp.write_text(fixture, encoding="utf-8")
        for platform in [0, 1]:
            exe = Path(temp) / ("fixture-" + str(platform) + ".exe")
            subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-Wall", "-Wextra", "-Werror",
                            "-Wno-missing-field-initializers", "-pthread", "-DTEST_DMA_FALLBACK=" + str(platform),
                            "-I" + str(a.headers), "-I" + str(ROOT / "gpu/vk"),
                            *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"] if a.sanitize else []),
                            str(cpp), "-o", str(exe)], check=True)
            for route, case in cases:
                subprocess.run([str(exe), route, case], check=True, timeout=15)
            if platform:
                for case in ["dma-bind-fallback", "dma-create-fallback", "dma-allocate-fallback", "both-bind-fail", "nvidia"]:
                    subprocess.run([str(exe), "pool", case], check=True, timeout=15)
    print(f"{len(cases) * 2 + 5} factory/rollback/fallback/hold cases passed")


if __name__ == "__main__":
    main()
