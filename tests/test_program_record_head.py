"""A program record's head is read whole by both readers of hookshot_programs.bin.

program_cache_save writes, before a program's stages, its kind, texture answers and questions, workgroup, shared memory and the
mesh fallback's five words. program_cache_read read all of them; program_cache_each (the prediction's, vk_predict.cpp) stopped
after shared memory and took the fallback's first two words for the stage count: 0 for every program that is not a fallback one,
so no kept program reached the prediction ("predicted 0 pipelines ... for 0 program variants"). The head is now written by one
function (put_head) and read by one (get_head). This builds put/put_list/Reader/put_head/get_head as they are in the source, with
a stand-in Program, and checks that a fallback and an ordinary program each come back whole with the stage count after them; and
that program_cache_save, program_cache_read and program_cache_each use those two functions.
python -I -B tests/test_program_record_head.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)

SHADER = ROOT / "gpu" / "vk" / "vk_shader.cpp"


def block(text, marker, end="}"):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at] + (";" if end == "};" else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="g++")
    a = ap.parse_args()
    src = _with_cache(SHADER)
    parts = [
        block(src, "void put_bytes("),
        src[src.index("template <class T> void put("):src.index("\n", src.index("template <class T> void put("))],
        block(src, "template <class V> void put_list("),
        block(src, "struct Reader {", "};"),
        block(src, "void put_head("),
        block(src, "void get_head("),
    ]
    cpp = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
enum VkPrimitiveTopology { VK_PRIMITIVE_TOPOLOGY_POINT_LIST = 0, VK_PRIMITIVE_TOPOLOGY_LINE_LIST = 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3 };
struct Program {   // (the fields of vk_shader.h's Program that a record's head holds)
    bool compute = false, mesh = false, fallback = false, fallback_task = false;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    VkPrimitiveTopology fallback_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t fallback_vertices_per_workgroup = 1, fallback_record_words = 1, fallback_payload_words = 0, fallback_task_threads = 1;
    struct Answer {
        uint16_t stage, desc;
        uint8_t type;
        bool image = false;
    };
    std::vector<Answer> answers;
    std::vector<uint8_t> questions;
};
""" + "\n".join(parts) + r"""
int fails = 0;
#define WANT(c) do { if (!(c)) std::printf("%s: %s\n", name, #c), fails++; } while (0)
void check(const char* name, const Program& p, uint64_t stages) {
    std::vector<uint8_t> o;
    put_head(o, p);
    put(o, stages);   // (as program_cache_save: the stage count right after the head)
    Reader r{o.data(), o.data() + o.size()};
    Program q;
    get_head(r, q);
    WANT(r.get<uint64_t>() == stages);
    WANT(!r.bad && r.p == r.end);
    WANT(q.compute == p.compute && q.mesh == p.mesh && q.fallback == p.fallback && q.fallback_task == p.fallback_task);
    WANT(q.workgroup == p.workgroup && q.shared_memory == p.shared_memory);
    WANT(q.fallback_topology == p.fallback_topology && q.fallback_vertices_per_workgroup == p.fallback_vertices_per_workgroup);
    WANT(q.fallback_record_words == p.fallback_record_words && q.fallback_payload_words == p.fallback_payload_words);
    WANT(q.fallback_task_threads == p.fallback_task_threads);
    WANT(q.answers.size() == p.answers.size() && q.questions == p.questions);
    for (size_t i = 0; i < q.answers.size() && i < p.answers.size(); i++)
        WANT(q.answers[i].stage == p.answers[i].stage && q.answers[i].desc == p.answers[i].desc && q.answers[i].type == p.answers[i].type);
}
int main() {
    Program plain;
    plain.answers = {{1, 3, 0x81, false}};
    plain.questions = {1, 0, 2};
    plain.shared_memory = 256;
    check("graphics", plain, 2);
    Program fb;
    fb.mesh = false, fb.fallback = true, fb.fallback_task = true;
    fb.workgroup = {32, 1, 1};
    fb.fallback_topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    fb.fallback_vertices_per_workgroup = 384, fb.fallback_record_words = 9, fb.fallback_payload_words = 4, fb.fallback_task_threads = 8;
    check("mesh fallback", fb, 4);
    if (fails) return 1;
    std::puts("ok");
}
"""
    with tempfile.TemporaryDirectory(prefix="program-head-") as tmp:
        c, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        c.write_text(cpp, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", str(c), "-o", str(exe)], check=True)
        got = subprocess.run([str(exe)], capture_output=True, text=True)
        if got.returncode or got.stdout.strip() != "ok":
            raise SystemExit("a program record's head:\n" + got.stdout + got.stderr)
    for marker, want in (("void program_cache_save(", "put_head(o, p);"), ("void program_cache_read(", "get_head(r, *p);"),
                         ("void program_cache_each(", "get_head(r, p);")):
        if want not in block(src, marker):
            raise SystemExit("%s does not use %s" % (marker.rstrip("("), want))
    print("PASS: a program record's head comes back whole, fallback or not, in both readers")


if __name__ == "__main__":
    main()
