"""Live final stereo mixing: safe averages, non-Mono identity, and unchanged surround.

Compile the real mixer helper, and check its live option, menu row and default.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    source = read("host/nn_audio.c")
    start = source.index("static void audio_mono(")
    function = source[start:source.index("\n}", start) + 2]
    loop = source[source.index("static DWORD WINAPI renderer_main"):]
    mix_call = re.search(r"audio_mono\(pcm, FRAME, out_channels, [^;]+;", loop).group(0)
    fixture = r"""
#include <stdint.h>
#include <assert.h>
#include <string.h>
FUNCTION
enum { HOS_OPT_AUDIO_OUTPUT = 57 };
static int output_choice;
static int rx_option(int id, int otherwise) {
    assert(id == HOS_OPT_AUDIO_OUTPUT && otherwise == 0);
    return output_choice;
}
static void mix_output(int16_t *pcm, int FRAME, int out_channels) {
    MIX_CALL
}
int main(void) {
    const int16_t original[] = {32767,32767,-32768,-32768,32767,-32768,0,2000,-3000,1000,0,0};
    const int16_t expected[] = {32767,32767,-32768,-32768,0,0,1000,1000,-1000,-1000,0,0};
    int16_t pcm[12];
    memcpy(pcm, original, sizeof pcm);
    audio_mono(pcm, 6, 2, 0);
    assert(memcmp(pcm, original, sizeof pcm) == 0);
    audio_mono(pcm, 6, 2, 1);
    assert(memcmp(pcm, expected, sizeof pcm) == 0);
    /* Turning Off on the next frame restores stereo, without a restart. */
    memcpy(pcm, original, sizeof pcm);
    audio_mono(pcm, 6, 2, 0);
    assert(memcmp(pcm, original, sizeof pcm) == 0);
    audio_mono(pcm, 2, 6, 1);
    assert(memcmp(pcm, original, sizeof pcm) == 0);
    audio_mono(pcm, 0, 2, 1);
    assert(memcmp(pcm, original, sizeof pcm) == 0);
    /* The renderer's actual option expression: Auto never means Mono, even on stereo. */
    const int choices[] = {0, 1, 2, 3, 1, 3, 0};
    for (unsigned i = 0; i < sizeof choices / sizeof *choices; i++) {
        output_choice = choices[i];
        memcpy(pcm, original, sizeof pcm);
        mix_output(pcm, 6, 2);
        assert(memcmp(pcm, output_choice == 3 ? expected : original, sizeof pcm) == 0);
        memcpy(pcm, original, sizeof pcm);
        mix_output(pcm, 2, 6);
        assert(memcmp(pcm, original, sizeof pcm) == 0);
    }
}
""".replace("FUNCTION", function).replace("MIX_CALL", mix_call)
    mix = loop.index("audio_mono(pcm, FRAME, out_channels, rx_option(HOS_OPT_AUDIO_OUTPUT, 0) == 3)")
    assert loop.index("audio_apply_gain(pcm") < mix < loop.index("output_frame(pcm)")
    assert "if (id == HOS_OPT_AUDIO_OUTPUT) return opts.audio_output;" in read("gpu/vk/vk_replay.cpp")
    assert re.search(r"int audio_output\s*=\s*0;", read("gpu/vk/vk_menu.h"))
    assert re.search(
        r'\{TAB_AUDIO, "Audio output", &opts.audio_output, 0, 3, 1, tx_audio_output,\s*'
        r'"[^"]+", hn_audio_output, true, true\}', read("gpu/vk/vk_menu.cpp"))
    assert "mono_audio" not in read("gpu/vk/vk_menu.h")
    assert '"Mono audio"' not in read("gpu/vk/vk_menu.cpp")
    assert "HOS_OPT_MONO_AUDIO" not in read("gpu/vk/vk_api.h")
    # Main has assigned the fork's original id 46 to Button prompts.
    ids = re.findall(r"(HOS_OPT_\w+)\s*=\s*(\d+)", read("gpu/vk/vk_api.h"))
    mono_id = dict(ids)["HOS_OPT_AUDIO_OUTPUT"]
    assert [name for name, value in ids if value == mono_id] == ["HOS_OPT_AUDIO_OUTPUT"]
    with tempfile.TemporaryDirectory() as directory:
        c = Path(directory) / "mono.c"
        c.write_text(fixture, encoding="utf-8")
        exe = Path(directory) / ("mono.exe" if os.name == "nt" else "mono")
        subprocess.run([args.cc, "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: live mono, extremes, silence, non-Mono identity, surround unchanged, menu wiring and default")


if __name__ == "__main__":
    main()
