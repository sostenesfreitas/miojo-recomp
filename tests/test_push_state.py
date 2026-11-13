"""The pushed descriptors are remembered as one vector of words, compared and kept in place.

A program change pushes every write, and the next draws of it push the writes that changed. The old bookkeeping was a vector of
vectors resized to each program's writes, so a program with more writes than the one before it grew new vectors word by word.
python -I -B tests/test_push_state.py [--cxx g++]
"""
import argparse
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
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    old = block(source, "void write_words(")
    app = block(source, "void words_append(")
    same = block(source, "int words_same_store(")
    bind = block(source, "bool bind_descriptors(")
    if "RX_VK_PUSH_STATE_OLD" not in bind:
        raise SystemExit("the old bookkeeping has no switch")
    full = bind[: bind.index("static thread_local std::vector<VkWriteDescriptorSet> changed")]
    if "ps.words.clear(), ps.at.clear()" not in full or "ps.n = dw().writes.size()" not in full:
        raise SystemExit("a full push does not remember its words")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
typedef uint64_t VkBuffer; typedef uint64_t VkSampler; typedef uint64_t VkImageView; typedef uint64_t VkBufferView;
typedef uint64_t VkDeviceSize;
struct VkDescriptorBufferInfo {{ VkBuffer buffer; VkDeviceSize offset, range; }};
struct VkDescriptorImageInfo {{ VkSampler sampler; VkImageView imageView; int imageLayout; }};
struct VkWriteDescriptorSet {{
    uint32_t dstBinding, descriptorCount, descriptorType;
    const VkDescriptorImageInfo* pImageInfo; const VkDescriptorBufferInfo* pBufferInfo; const VkBufferView* pTexelBufferView;
}};
{old}
{app}
{same}
int main() {{
    VkDescriptorBufferInfo b[2] = {{{{1, 2, 3}}, {{4, 5, 6}}}};
    VkDescriptorImageInfo i[1] = {{{{7, 8, 1}}}};
    VkBufferView t[1] = {{9}};
    VkWriteDescriptorSet w[3] = {{}};
    w[0].dstBinding = 0, w[0].descriptorCount = 2, w[0].descriptorType = 6, w[0].pBufferInfo = b;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = 1, w[1].pImageInfo = i;
    w[2].dstBinding = 2, w[2].descriptorCount = 1, w[2].descriptorType = 4, w[2].pTexelBufferView = t;
    std::vector<uint64_t> words, old_words, one;
    std::vector<uint32_t> at;
    for (auto& x : w) at.push_back((uint32_t)words.size()), words_append(x, words);
    for (auto& x : w) write_words(x, one), old_words.insert(old_words.end(), one.begin(), one.end());
    CHECK(words == old_words);                       // (the same words as the vector of vectors held)
    CHECK(at.size() == 3 && at[1] == 7 && at[2] == 10 && words.size() == 12);
    const uint64_t* e = words.data();
    auto same = [&](int k) {{ return words_same_store(w[k], words.data() + at[k], e + (k < 2 ? at[k + 1] : words.size())); }};
    for (int k = 0; k < 3; k++) CHECK(same(k) == 1);
    b[1].range = 60;                                 // (a buffer range changed: that write only)
    CHECK(same(0) == 0);
    CHECK(same(0) == 1);                             // (and kept: the same again)
    CHECK(same(1) == 1 && same(2) == 1);
    i[0].imageView = 80;
    CHECK(same(1) == 0 && same(1) == 1);
    t[0] = 90;
    CHECK(same(2) == 0 && same(2) == 1);
    CHECK(words[at[0] + 6] == 60 && words[at[1] + 2] == 80 && words[at[2] + 1] == 90 && words.size() == 12);
    w[0].descriptorCount = 1;                        // (a write that is not the one the words were made for: refused, nothing touched)
    const std::vector<uint64_t> before = words;
    CHECK(same(0) == -1 && words == before);
    std::printf("PASS: pushed descriptors are compared and kept in one vector\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="push-state-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
