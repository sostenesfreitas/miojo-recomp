"""A replay read reuses the last answer for that page until a stamp or a new capture.

Once a capture exists, every uniform word took the pool lock to ask whether its page
was in one. The answers are kept per page, 32 a thread: a handle program reads a word
of a uniform buffer on one page and a word of a storage buffer on another, and with
one page kept every such read missed. Only "in no capture" is kept: a captured page's
words are copied from the capture at each read, not a 4 KB copy of the page a thread.
python -I -B tests/test_submitted_page.py [--cxx g++]
"""
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
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    fn = block(source, "bool submitted_page_reuse(")
    read = block(source, "bool memory_read_as_submitted(")
    reuse_at = read.index("submitted_page_reuse(")
    lock_at = read.index("std::lock_guard lk(pools_mu)")
    if reuse_at > lock_at:
        raise SystemExit("the page is looked up under the pool lock before the cache")
    if read.count("submitted_page_for(page0)") != 1:
        raise SystemExit("memory_read_as_submitted does not look its page up in the per-page cache")
    if "page0 == page1 && from == 0" not in read:
        raise SystemExit("memory_read_as_submitted keeps an answer for a page a capture holds")
    struct = block(source, "struct SubmittedPage {") + ";"
    count = source[source.index("constexpr int kSubmittedPages"):]
    count = count[:count.index(";") + 1]
    slot = block(source, "inline SubmittedPage& submitted_page_for(")
    pages = source[source.index("thread_local SubmittedPage submitted_pages["):]
    pages = pages[:pages.index(";") + 1]
    for name in ("void memory_stamp(", "void memory_collect_written(", "bool take_capture("):
        body = block(source, name)
        if "cap_view_bump()" not in body:
            raise SystemExit(f"{name} changes page state without dropping the cached read")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
{struct}
{count}
{pages}
{slot}
int main() {{
    CHECK(submitted_page_reuse(9, 4, 7, 9, 4, 7));
    CHECK(!submitted_page_reuse(9, 4, 8, 9, 4, 7));
    CHECK(!submitted_page_reuse(9, 5, 7, 9, 4, 7));
    CHECK(!submitted_page_reuse(10, 4, 7, 9, 4, 7));
    CHECK(!submitted_page_reuse(9, 4, 7, 9, 4, 0));
    // (a page and its neighbour, and pages a slot-count apart, keep separate answers: a uniform and a storage word read in
    // turn hit both)
    if (std::getenv("RX_VK_SUBMITTED_PAGES")) {{   // (RX_VK_SUBMITTED_PAGES=1: every page in one slot, as before #1762)
        for (uint64_t p = 0; p < 4096; p++) CHECK(&submitted_page_for(p) == &submitted_pages[0]);
        std::printf("PASS: RX_VK_SUBMITTED_PAGES=1 keeps one slot\\n");
        return 0;
    }}
    CHECK((kSubmittedPages & (kSubmittedPages - 1)) == 0);
    // (no copy of a page in a slot: the table is a few hundred bytes of TLS a thread, not 128 KB)
    CHECK(sizeof(submitted_pages) <= 1024);
    for (uint64_t p = 0; p < (1u << 20); p++) {{
        CHECK(&submitted_page_for(p) != &submitted_page_for(p + 1));
        CHECK(&submitted_page_for(p) != &submitted_page_for(p + kSubmittedPages));
    }}
    std::printf("PASS: a captured page is reused only for that generation; neighbouring pages keep their own; no page copies kept\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="submitted-page-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
        subprocess.run([str(exe)], check=True, env=dict(os.environ, RX_VK_SUBMITTED_PAGES="1"))


if __name__ == "__main__":
    main()
