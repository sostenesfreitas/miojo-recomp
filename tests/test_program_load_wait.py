"""The load a create call queues and the first draw that needs it (gpu/vk/vk_programs.cpp, RX_VK_LOAD=create): LoadJob, load_done,
load_publish and programs_load_wait, cut from the source and run on stand-ins. A first use before any worker began the job runs it
itself, one that comes while a worker runs it waits; either way what the job read is in the programs' lists exactly once when the
wait returns, so the pick finds it and translates nothing; a job that read nothing leaves the first-use path. And the source: a
program cache load asks for a queued load first, and a job only ever publishes finished programs. No game or GPU is needed.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    src = (ROOT / "gpu/vk/vk_programs.cpp").read_text(encoding="utf-8")
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    load = block(cache, "void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep) {")
    if "if (!keep) programs_load_wait(key);" not in load or load.index("programs_load_wait(key)") > load.index("disk_records.find(key)"):
        raise SystemExit("program_cache_load does not ask for a load queued at the program's create call before it looks for records")
    work = block(src, "void load_work(LoadJob& job) {")
    for bad in ("translate", "seed_", "shader_mu", "programs["):
        if bad in work:
            raise SystemExit("load_work must only read prepared data, with no lock a draw holds: found `%s`" % bad)
    worker = block(src, "void load_worker(int index) {")
    if "shader_mu" in worker or "load_publish" in worker or "shader_mu" in block(src, "void load_done(LoadJob& job) {"):
        raise SystemExit("a load worker takes the picks' lock: a pick could wait for it behind a low-priority thread")
    if "p->ready" in work or "ready.store(false" in src:
        raise SystemExit("a load job must publish only finished programs: a draw uses a program it waited for as it is")
    types = block(src, "struct LoadJob {") + ";\n" + src[src.index("std::mutex jobs_mu;"):src.index("bool workers_started")]
    functions = "\n".join(block(src, m) for m in ("void load_done(LoadJob& job) {", "void load_publish(const std::shared_ptr<LoadJob>& job) {",
                                                  "void programs_load_wait(uint64_t key) {"))
    with tempfile.TemporaryDirectory(prefix="program-load-wait-") as tmp:
        tmp = Path(tmp)
        (tmp / "load_types.inc").write_text(types, encoding="utf-8")
        (tmp / "load_functions.inc").write_text(functions, encoding="utf-8")
        exe = tmp / "program-load-wait"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-pthread", "-I" + str(tmp),
                        str(ROOT / "tests/program_load_wait_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
