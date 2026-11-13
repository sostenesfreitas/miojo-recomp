"""Auto's speakers follow the default output's speaker positions, not its channel count (host/nn_audio.c, issue 2182).

The real au_layout_channels from host/nn_audio.c is compiled with a small driver and asked about mix formats Windows
reports: a device with more channels than 5.1 speakers (an audio interface's 8 outputs with channel mask 0, quad, a
format with no mask) counts as stereo, so the game's centre and rear sounds are not sent to outputs nothing listens to.
No device is opened.

python -I -B tests/test_audio_speaker_layout.py [--cxx clang++]   (--cxx cl: MSVC, from a developer prompt)
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include <cstdlib>
HELPER
int main(int argc, char **argv) {
    int failed = 0;
    for (int i = 1; i + 3 < argc; i += 4) {
        const int channels = atoi(argv[i]), extensible = atoi(argv[i + 1]), want = atoi(argv[i + 3]);
        const unsigned long mask = strtoul(argv[i + 2], nullptr, 0);
        const int got = au_layout_channels(channels, extensible, mask);
        if (got != want) {
            fprintf(stderr, "FAIL: %d channels, extensible %d, mask %#lx: %d (want %d)\n", channels, extensible, mask, got, want);
            failed = 1;
        }
    }
    return failed;
}
"""

# (channels, WAVE_FORMAT_EXTENSIBLE, channel mask, the channels Auto counts)
CASES = (
    (2, 1, 0x3, 2),       # stereo
    (2, 0, 0x0, 2),       # stereo, plain WAVEFORMATEX
    (1, 1, 0x4, 2),       # mono
    (6, 1, 0x3F, 6),      # 5.1 (back)
    (6, 1, 0x60F, 6),     # 5.1 (side)
    (8, 1, 0x63F, 8),     # 7.1
    (8, 1, 0x0, 2),       # an audio interface's 8 outputs, no speaker named (a Scarlett 18i8: issue 2182)
    (18, 1, 0x0, 2),      # a bigger interface
    (4, 1, 0x33, 2),      # quad: no centre
    (4, 1, 0x107, 2),     # FL FR FC BC: one channel behind
    (6, 1, 0x3B, 2),      # 5.1 without its centre
    (6, 0, 0x0, 2),       # more than two channels without a mask
    (8, 1, 0xFF, 8),      # 7.1 wide (front centres): has 5.1 in it
)


def between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    audio = (ROOT / "host/nn_audio.c").read_text(encoding="utf-8")
    helper = between(audio, "/* (hookshot) The channels of a mix format", "static int au_device_channels(void) {")
    assert "static int au_layout_channels(" in helper, "host/nn_audio.c has no au_layout_channels"
    # (the device's channels go through it: a mix format's count alone is what issue 2182 was)
    device = between(audio, "static int au_device_channels(void) {", "#else")
    assert "au_layout_channels(mix->nChannels" in device, "au_device_channels does not ask au_layout_channels"
    with tempfile.TemporaryDirectory(prefix="audio-speaker-layout-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(FIXTURE.replace("HELPER", helper), encoding="utf-8")
        if os.path.splitext(os.path.basename(args.cxx))[0].lower() == "cl":
            command = [args.cxx, "/nologo", "/std:c++17", "/W4", "/WX", "/EHsc", str(source), "/Fe:" + str(exe),
                       "/Fo:" + str(work) + os.sep]
        else:
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)]
        subprocess.run(command, check=True, cwd=work)
        cases = [arg for channels, extensible, mask, want in CASES
                 for arg in (str(channels), str(extensible), hex(mask), str(want))]
        subprocess.run([str(exe)] + cases, check=True)
    print("PASS: Auto counts 5.1 speakers, not channels: an audio interface's outputs, quad or no mask are stereo")


if __name__ == "__main__":
    main()
