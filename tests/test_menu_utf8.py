"""The Insert menu's UTF-8 text (a microphone's name can be in any script; #1389 made the old Linux menu draw it). Dear ImGui
draws UTF-8 as it is; what the menu does to text itself must keep whole characters: the cut and slid labels (char_starts,
for fit), the Microphone row's name cut to its buffer (utf8_whole) and the saved name cut to its key (text_char_start,
mic_key). The real functions from gpu/vk/vk_menu.cpp, compiled with a fixture; no ImGui, GPU or game.

    python -I -B tests/test_menu_utf8.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    parts = [block(src, m) for m in ("size_t text_char_start(", "std::string mic_key(", "void utf8_whole(",
                                     "std::vector<size_t> char_starts(")]
    fixture = r'''#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
constexpr size_t kMicNameKept = 60;
''' + "\n".join(parts) + r'''
/// Whether s is whole UTF-8: every lead byte has all its continuation bytes
bool whole(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}
int main() {
    const std::string mixed = "a\xC3\xA9\xE6\x97\xA5\xF0\x9F\x8E\xA4z";   // (a, e acute, a CJK character, a microphone emoji, z)
    const std::vector<size_t> at = char_starts(mixed);
    CHECK((at == std::vector<size_t>{0, 1, 3, 6, 10, 11}));
    for (size_t i = 0; i + 1 < at.size(); i++)   // (every cut and slide at a start: whole characters on both sides)
        for (size_t j = i; j < at.size(); j++) CHECK(whole(mixed.substr(at[i], at[j] - at[i])));
    CHECK(char_starts("") == std::vector<size_t>{0});
    // (a name cut by snprintf inside a character: cut back to the last whole one)
    for (size_t n = 1; n <= mixed.size(); n++) {
        char buf[32];
        snprintf(buf, n + 1, "%s", mixed.c_str());
        utf8_whole(buf);
        CHECK(whole(buf));
        CHECK(strlen(buf) + 3 >= n);   // (no more than one character's bytes dropped)
    }
    // (the saved key: at most kMicNameKept bytes, whole characters)
    std::string name;
    for (int i = 0; i < 40; i++) name += "\xD0\x96";   // (Cyrillic, two bytes each: 80 bytes)
    const std::string key = mic_key(name.c_str());
    CHECK(key.size() <= kMicNameKept && whole(key) && key.size() == 60);
    name = "x" + name;   // (now the 60th byte is inside a character)
    const std::string key2 = mic_key(name.c_str());
    CHECK(key2.size() <= kMicNameKept && whole(key2) && key2.size() == 59);
    CHECK(mic_key(nullptr).empty() && mic_key("USB Mic") == "USB Mic");
    // (a device's name with a line break would have added lines to hookshot_options.ini)
    CHECK(mic_key("Mic\noutput_res=2\r") == "Mic output_res=2");   // (the line break a space, the end trimmed)
    std::printf("PASS: the menu's text cut and slid by whole UTF-8 characters\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="menu-utf8-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation", str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
