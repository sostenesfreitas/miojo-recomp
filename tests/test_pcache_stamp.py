"""The pipeline cache's files are dropped when they were made for another translator (gpu/vk/pcache_stamp.h; no game/GPU
required): the header compiled and run against a temporary folder, and a check that vk_draw.cpp's pcache_init asks it with
program_cache_stamp before the cache file is read, keeps RX_VK_PCACHE_LOAD=0 as it was, and has RX_VK_PCACHE_STAMP=0 to keep
the files whatever made them.

python -I -B tests/test_pcache_stamp.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    if '#include "pcache_stamp.h"' not in draw:
        raise SystemExit("vk_draw.cpp does not include pcache_stamp.h")
    drop = block(draw, "bool pcache_drop_stale()")
    for needle in ('getenv("RX_VK_PCACHE_STAMP")', "if (pcache_load_off() || !by_stamp) return false;",
                   "pcache_stamp::check(dir, program_cache_stamp())", "return gone.files > 0;"):
        if needle not in drop:
            raise SystemExit("vk_draw.cpp: pcache_drop_stale() no longer has: " + needle)
    init = block(draw, "void pcache_init()")
    # (the stale files go before anything reads them: the main file here, the play files in play_files_merge)
    if not init.index("pcache_drop_stale();") < init.index("pcache_file_read(pcache_path)") < init.index("play_files_merge()"):
        raise SystemExit("vk_draw.cpp: pcache_init() reads the cache's files before it drops those of another translator")
    # (the warm pass keeps the stage libraries in the dropped file: its record must not say they are made)
    prep = block(draw, "bool prepare_run(")
    reset = "if (!background && pcache_drop_stale() && rewarm_once.exchange(false)) prepared_for.warmed = 0;"
    if reset not in prep or not prep.index("prepare_check(") < prep.index(reset) < prep.index("prepared_for.warmed == warmed"):
        raise SystemExit("vk_draw.cpp: prepare_run() keeps the warm record when the pipeline cache's files were dropped "
                         "(the stage libraries went with them and were not made again)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pcache-stamp-") as tmp:
        exe = Path(tmp) / "pcache-stamp"
        work = Path(tmp) / "folder"
        work.mkdir()
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_pcache_stamp.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe), str(work)], check=True)


if __name__ == "__main__":
    main()
