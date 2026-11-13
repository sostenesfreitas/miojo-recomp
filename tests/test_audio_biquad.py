"""Run the real voice/splitter DSP and queued filter APIs against authored signals.

No game, GPU or audio device. --source also accepts a host with unimplemented float
entry points: use no-op stubs for those, as the generated runtime stubs do.
The extracted DSP keeps its real env_on dependency from hookshot_env.h.
"""
import argparse
from pathlib import Path
import shutil
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
    parser.add_argument('--source', type=Path, default=ROOT / 'host/nn_audio.c')
    parser.add_argument('--cc', default=None)
    args = parser.parse_args()
    compiler = args.cc or shutil.which('cc') or shutil.which('clang')
    if not compiler:
        print('SKIP: no C compiler')
        return
    source = args.source.read_text(encoding='utf-8')
    chunks = [
        source[source.index('#define FRAME'):source.index('static int mix_depth(')],
        source[source.index('/* ---- parameter updates'):source.index("/* The mixer's next deadline.")],
        source[source.index('/* ---- splitters '):source.index('/* ---- the device sink:')],
        function(source, 'HLE(_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_21BiquadFilterParameterE)'),
    ]
    voice = '_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_22TBiquadFilterParameterIfEE'
    chunks.append(function(source, 'HLE(' + voice + ')') if 'HLE(' + voice + ')' in source else 'HLE(' + voice + ') {}')
    for mix in ('10SubMixType', '12FinalMixType'):
        name = '_ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_' + mix + 'EiRKNS0_22TBiquadFilterParameterIfEE'
        if 'HLE(' + name + ')' not in source:
            chunks.append('HLE(' + name + ') {}')
    with tempfile.TemporaryDirectory(prefix='audio-biquad-') as tmp:
        folder = Path(tmp)
        (folder / 'audio_biquad.inc').write_text('\n'.join(chunks), encoding='utf-8')
        exe = folder / 'fixture.exe'
        subprocess.run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-include', str(ROOT / 'gpu/vk/compat/hookshot_env.h'),
                        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                        '-I' + str(folder), str(ROOT / 'tests/audio_biquad_test.c'), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
