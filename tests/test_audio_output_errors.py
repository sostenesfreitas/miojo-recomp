"""Compile the real output section with synthetic audio/device spies, without opening a device.

python -I -B tests/test_audio_output_errors.py [--cc clang]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]
CASES = ("normal", "busy", "prepare", "write", "unprepare", "open", "bounded", "stall", "surround", "fallback",
         "lost", "gone", "service", "default")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_audio.c")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    process_flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0

    def run(command):
        result = subprocess.run(command, creationflags=process_flags, capture_output=True,
                                text=True, encoding="utf-8", errors="replace")
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()
    source = args.source.read_text(encoding="utf-8")
    definitions = []
    for name in ("FRAME", "RATE"):
        matches = re.findall(r"^#define " + name + r"\s+\d+[^\n]*", source, re.M)
        assert len(matches) == 1, name
        definitions.extend(matches)
    start, end = source.index("#define NBLOCKS"), source.index("/* ---- parameter updates")
    actual = "\n".join(definitions) + "\n" + source[start:end]
    temporary = tempfile.TemporaryDirectory(prefix="audio-output-")
    folder = Path(temporary.name).resolve()
    try:
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("audio-output-")
        (folder / "audio_output.inc").write_text(actual, encoding="utf-8")
        executable = folder / "fixture.exe"
        run([args.cc, "-std=c11", "-include", ENV_ON_H, "-Wall", "-Wextra", "-Werror", "-I" + str(folder),
             str(ROOT / "tests/audio_output_errors_test.c"), "-o", str(executable)])
        for case in ((args.case,) if args.case else CASES):
            run([str(executable), case])
    finally:
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("audio-output-")
        temporary.cleanup()


if __name__ == "__main__":
    main()
