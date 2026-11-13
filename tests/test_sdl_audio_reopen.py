"""Linux's sound output opened again (host/nn_audio.c output_reopen after 1.5 s of no sound, output_move when the default
device changes): host/posix/win32_posix.c waveOutClose closes the SDL device host/hos_sdl.c opened. It only emptied the
queue, so the first device stayed open and the default output refused a second open: the sound stayed off for the run.

Builds the real waveOut calls and SDL adapter over the system's SDL with its dummy driver (no sound card, nothing
heard): opened, a block played, closed and opened again four times, each new device taking its block. Skipped where
no SDL3 or SDL2 library is installed.

    python -I -B tests/test_sdl_audio_reopen.py [--cc gcc]
"""
import argparse
import ctypes.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "Linux's waveOut on SDL (host/posix): Windows plays through its own"}

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }

static int played(WAVEHDR *h) {   /* (the device's callback took the block within 2 s) */
    for (int i = 0; i < 400; i++) {
        if (__atomic_load_n(&h->dwFlags, __ATOMIC_ACQUIRE) & WHDR_DONE) return 1;
        nanosleep(&(struct timespec){0, 5000000}, NULL);
    }
    return 0;
}

int main(void) {
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM, f.nChannels = 2, f.nSamplesPerSec = 48000, f.wBitsPerSample = 16;
    f.nBlockAlign = 4, f.nAvgBytesPerSec = 48000 * 4;
    static short pcm[2 * 480];
    for (int i = 0; i < 4; i++) {
        HWAVEOUT w = NULL;
        if (waveOutOpen(&w, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            printf("FAIL: open %d of the output was refused (the one before still open)\n", i + 1);
            return 1;
        }
        WAVEHDR h = {0};
        h.lpData = (LPSTR)pcm, h.dwBufferLength = sizeof pcm;
        waveOutPrepareHeader(w, &h, sizeof h);
        waveOutWrite(w, &h, sizeof h);
        if (!played(&h)) {
            printf("FAIL: open %d took no sound\n", i + 1);
            return 1;
        }
        waveOutReset(w);
        waveOutUnprepareHeader(w, &h, sizeof h);
        if (waveOutClose(w) != MMSYSERR_NOERROR) {
            printf("FAIL: close %d\n", i + 1);
            return 1;
        }
    }
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    if not sys.platform.startswith("linux"):
        print("SKIP: Linux's waveOut on SDL runs on Linux")
        return
    if not (ctypes.util.find_library("SDL3") or ctypes.util.find_library("SDL2-2.0")):
        print("SKIP: no SDL3 or SDL2 library installed")
        return
    with tempfile.TemporaryDirectory(prefix="sdl-audio-reopen-") as tmp:
        tmp = Path(tmp)
        # (hos_sdl.c beside a hos.h of its own: the real one needs the lifted game's headers, and hos_log is all it takes)
        (tmp / "hos.h").write_text("#define HOS_H\nvoid hos_log(const char *fmt, ...);\n", encoding="utf-8")
        (tmp / "hos_sdl.c").write_text((ROOT / "host/hos_sdl.c").read_text(encoding="utf-8"), encoding="utf-8")
        (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
        exe = tmp / "reopen"
        subprocess.run([a.cc, "-std=gnu11", "-D_GNU_SOURCE", "-w", "-I", str(tmp), "-I", str(ROOT / "host/posix"),
                        "-I", str(ROOT / "host"), str(tmp / "driver.c"), str(tmp / "hos_sdl.c"),
                        str(ROOT / "host/posix/win32_posix.c"), "-o", str(exe), "-lpthread", "-ldl", "-lm"], check=True)
        env = dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
        if "sdl: no SDL3 or SDL2 found" in r.stderr:
            print("SKIP: SDL could not be loaded")
            return
        if r.returncode:
            raise SystemExit("FAIL: the sound output opened again:\n" + r.stdout + r.stderr)
    print("PASS: Linux's sound output closes its SDL device: opened again four times, each takes its sound")


if __name__ == "__main__":
    main()
