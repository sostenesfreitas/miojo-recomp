"""Actual native cache storage: ownership, byte parity, publication and retry."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-cache-file-") as directory:
        path = Path(directory).resolve()
        executable = path / "check.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "gpu/vk"), str(ROOT / "tests/pipeline_cache_file_test.cpp"),
                        "-o", str(executable)], check=True, timeout=60)
        subprocess.run([str(executable), str(path)], check=True, timeout=30)
        fault_area = path / "native-faults"
        fault_area.mkdir()
        fault_executable = fault_area / "check.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "gpu/vk"), str(ROOT / "tests/pipeline_cache_io_fault_test.cpp"),
                        "-o", str(fault_executable)], check=True, timeout=60)
        subprocess.run([str(fault_executable), str(fault_area)], check=True, timeout=30)
        header = (ROOT / "gpu/vk/pipeline_cache_file.h").read_text(encoding="utf-8")
        mutations = {
            "capacity-instead-of-actual-length": (
                header.replace("!resize(actual)", "!resize(size_)"), "in.size()==128"),
            "unchecked-header": (
                header.replace("!valid(header, length)", "(static_cast<void>(valid), false)"),
                "PipelineCacheFile::read(path,valid).empty()"),
            "abandoned-temporary-leak": (
                header.replace("!DeleteFileW(temporary_wide_.c_str())", "false")
                      .replace("unlink(temporary_.c_str()) && errno != ENOENT", "false"), "leaked==0"),
        }
        for name, (mutated, assertion) in mutations.items():
            if mutated == header:
                raise AssertionError("mutation no longer applies: " + name)
            area = path / name
            area.mkdir()
            (area / "pipeline_cache_file.h").write_text(mutated, encoding="utf-8")
            executable = area / "check.exe"
            subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(area), str(ROOT / "tests/pipeline_cache_file_test.cpp"),
                            "-o", str(executable)], check=True, timeout=60)
            result = subprocess.run([str(executable), str(area)], capture_output=True,
                                    text=True, timeout=30)
            if result.returncode != 1 or "FAIL line" not in result.stderr or assertion not in result.stderr:
                raise AssertionError(f"mutation not causally rejected: {name}: {result.returncode}: {result.stderr}")
            print("Rejected semantic mutation:", name)


if __name__ == "__main__":
    main()
