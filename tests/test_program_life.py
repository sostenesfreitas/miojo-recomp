"""The states of what the game's programs need loaded (gpu/vk/program_life.h, used by gpu/vk/vk_programs.cpp): a load is queued
once when the game first makes a program of prepared stages and never for stages nothing was prepared for (a create never
translates); a first draw waits only for a load under way; a unit is let go by count only, when the game has freed every program
of it and it was drawn, the longest freed first; the next create loads it again. Compile and run the rule. No game or GPU is needed.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="program-life-") as tmp:
        exe = Path(tmp) / "program-life"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        str(ROOT / "tests/program_life_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
