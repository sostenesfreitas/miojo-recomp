"""Exercise the actual Aux API and mixer with synthetic signals; no game or audio device.

The extracted mixer keeps its real env_on dependency from hookshot_env.h.

python -I -B tests/test_audio_aux.py [--cc clang] [--source host/nn_audio.c]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source', type=Path, default=ROOT / 'host/nn_audio.c')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    size_api = function(source, 'HLE(_ZN2nn5audio43GetRequiredBufferSizeForAuxSendReturnBuffer')
    # Compile production types, graph update queue, effect processing and SDK entry points.
    chunks = [
        source[source.index('#define FRAME'):source.index('/* ---- wave buffers')],
        function(source, 'static void release_wb('),
        source[source.index('/* ---- I3DL2 reverb ('):source.index('/* ---- the mixer')],
        source[source.index('/* ---- parameter updates'):source.index("/* The mixer's next deadline.")],
        size_api,
    ]
    # Also accepts the old stubs so the first size/roundtrip checks demonstrate the original failure.
    start = source.index('/* ---- Aux API') if '/* ---- Aux API' in source else source.index('/* ---- effects: accepted')
    end = source.index('HLE(_ZN2nn5audio17IsReverbRemovable', start)
    chunks.append(source[start:end])
    start = source.index('static Reverb *reverb_of(')
    chunks.append(source[start:source.index('static void ret_float(', start)])
    with tempfile.TemporaryDirectory(prefix='audio-aux-') as tmp:
        directory = Path(tmp)
        # Run this regression first so an unmodified source fails behaviorally, before missing new APIs matter.
        sizing = directory / 'sizing.c'
        sizing.write_text('''#include <stdint.h>
#include <stdio.h>
typedef struct { uint64_t x[8]; } Ctx;
#define HLE(name) void size_api(Ctx *c)
#define ARG(n) c->x[n]
#define RET(v) (c->x[0] = (uint64_t)(v))
#define G32(p) (*(uint32_t *)(uintptr_t)(p))
''' + size_api + '''
int main(void) {
    uint32_t parameter[] = {48000, 240};
    Ctx c = {{(uintptr_t)parameter, 6, 5}};
    size_api(&c);
    if (c.x[0] != 28928) {
        fprintf(stderr, "FAIL: expected 28928 bytes for 6 channels x 5 frames, got %llu\\n",
                (unsigned long long)c.x[0]);
        return 1;
    }
    return 0;
}
''', encoding='utf-8')
        size_exe = directory / 'sizing.exe'
        subprocess.run([args.cc, '-std=c11', str(sizing), '-o', str(size_exe)], check=True)
        subprocess.run([str(size_exe)], check=True)
        (directory / 'audio_aux.inc').write_text('\n'.join(chunks), encoding='utf-8')
        exe = directory / 'audio_aux.exe'
        subprocess.run([args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-include', str(ROOT / 'gpu/vk/compat/hookshot_env.h'),
                        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                        '-I' + str(directory), str(ROOT / 'tests/audio_aux_test.c'), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
