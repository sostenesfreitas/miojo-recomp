"""Compile the actual wave-buffer calls, queued updates and the mixer's buffer consumption around an authored pass.

Ocarina's sound thread feeds six mono voices one after another: it reads each one's GetWaveBufferCount and appends
when one is short. The mixer finishing a buffer between two of those reads made one voice look short, the game
appended it an extra buffer, then one more each pass, until its queue was full and that speaker went silent. A
finished buffer must reach the game at the next RequestUpdateAudioRenderer, as on the console, never in between.
No game, SDK or output device is used. --source can select another nn_audio.c (the prior release fails the check).
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]
APPEND = "_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE"
RELEASED = "_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE"
COUNT = "_ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE"
STATE = "_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_audio.c")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("audio_block_helper", ROOT / "tests/test_audio_deadline.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    source = args.source.read_text(encoding="utf-8")
    constants = []
    for name in ("FRAME", "RATE", "AU_MAGIC", "MAX_VOICES", "MAX_MIXES", "MAX_SPLITTERS", "MAX_SPLIT_DESTS",
                 "MAX_CH", "MAX_BUF", "MAX_WB", "MAX_REL", "MAX_EFFECTS"):
        matches = re.findall(r"^#define " + name + r"\s+[^\n]+", source, re.M)
        assert len(matches) == 1, name
        constants.extend(matches)
    types = source[source.index("enum { K_VOICE"):source.index("static CRITICAL_SECTION lock;")]
    queue = source[source.index("enum { OP_VOICE_RELEASE"):source.index("static int64_t last_update_ns;")]
    actual = ["\n".join(constants), types, """
static Voice voices[MAX_VOICES];
static Mix mixes[MAX_MIXES];
static Splitter splitters[MAX_SPLITTERS];
static uint64_t split_bq_epoch;
static int sink_mix = -1;
/* (the samples are not looked at here: only which buffer the mixer is on) */
static void source_frame(Voice *v, const WB *w, int64_t s, float *out) { (void)v; (void)w; (void)s; out[0] = 0; }
static void adpcm_seek(Voice *v, const WB *w) { (void)v; (void)w; }
static int16_t adpcm_sample(Voice *v, const WB *w, int64_t s) { (void)v; (void)w; (void)s; return 0; }
"""]
    # The merged renderer's command application also owns Aux state. Include its
    # production declarations/reset helpers without replacing the wave-buffer code.
    aux_start = source.index("typedef struct {", source.index("/* ---- Aux:"))
    actual.append(source[aux_start:source.index("static int aux_ring_read(", aux_start)])
    actual.append(helper.block(source, "static void aux_reset("))
    actual.extend(helper.block(source, marker) for marker in (
        "static void put_handle(", "static int get_handle(", "static int voice_index(", "static Voice *voice_of(",
        "static WB read_wb(", "static void release_wb(", "static int next_source_frame("))
    actual.extend([queue, helper.block(source, "static void apply("), helper.block(source, "static void submit("),
                   helper.block(source, "static void commit(")])
    actual.extend(helper.block(source, "HLE(" + name + ")") for name in (APPEND, RELEASED, COUNT, STATE))
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="audio-wave-buffer-release-") as temporary:
        folder = Path(temporary)
        (folder / "actual_audio_wave_buffers.inc").write_text("\n\n".join(actual), encoding="utf-8")
        executable = folder / "fixture.exe"
        command = [args.cc, "-std=c11", "-include", ENV_ON_H, "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(folder),
                   str(ROOT / "tests/audio_wave_buffer_release_test.c"), "-lm", "-o", str(executable)]
        for command in (command, [str(executable)]):
            result = subprocess.run(command, creationflags=flags, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=30)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
