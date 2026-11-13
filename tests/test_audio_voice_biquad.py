"""Compile actual voice-filter setters, queued updates and the render_voice biquad loop.

Inputs and sample buffers are authored. No game, SDK, output device or resampler
is used. --source can select the prior release: its missing float import calls
the actual public runtime fallback, then fails the same queued-filter invariant.
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

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
FLOAT = "_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_22TBiquadFilterParameterIfEE"
LEGACY = "_ZN2nn5audio29SetVoiceBiquadFilterParameterEPNS0_9VoiceTypeEiRKNS0_21BiquadFilterParameterE"


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
                 "MAX_CH", "MAX_BUF", "MAX_WB", "MAX_REL"):
        matches = re.findall(r"^#define " + name + r"\s+[^\n]+", source, re.M)
        assert len(matches) == 1, name
        constants.extend(matches)
    types = source[source.index("enum { K_VOICE"):source.index("static CRITICAL_SECTION lock;")]
    queue = source[source.index("enum { OP_VOICE_RELEASE"):source.index("static int64_t last_update_ns;")]
    runtime = (ROOT / "third_party/a64-kit/rt/a64_rt.c").read_text(encoding="utf-8")
    actual = ["\n".join(constants), types, """
static Voice voices[MAX_VOICES];
static Mix mixes[MAX_MIXES];
static Splitter splitters[MAX_SPLITTERS];
static int sink_mix = -1;
static void release_wb(Voice *voice, uint64_t buffer) { (void)voice; (void)buffer; }
"""]
    actual.extend(helper.block(source, marker) for marker in (
        "static void put_handle(", "static int get_handle(", "static int voice_index(", "static Voice *voice_of("))
    if "static uint64_t split_bq_epoch;" in source:
        actual.append("static uint64_t split_bq_epoch;")
    if "} AuxRing;" in source:
        actual.append(next(line for line in source.splitlines() if line.startswith("#define MAX_EFFECTS")))
        aux_start = source.rfind("typedef struct {", 0, source.index("} AuxRing;"))
        actual.append(source[aux_start:source.index("static void aux_ring_reset(")])
        actual.extend(helper.block(source, marker) for marker in (
            "static void aux_ring_reset(", "static void aux_reset("))
    actual.extend([queue, helper.block(source, "static void apply("), helper.block(source, "static void submit("),
                   helper.block(source, "static void commit("), helper.block(runtime, "A64_WEAK void a64_unimplemented(")])
    for marker in ("static void read_float_biquad(", "static void biquad_frame("):
        if marker in source:
            actual.append(helper.block(source, marker))
    actual.append(helper.block(source, "HLE(" + LEGACY + ")"))
    try:
        actual.append(helper.block(source, "HLE(" + FLOAT + ")"))
    except ValueError:
        # An authored import dispatch for the absent HLE, using the real runtime fallback (not a replacement DSP/setter).
        actual.append('HLE(' + FLOAT + ') { static int seen; a64_unimplemented(c, "' + FLOAT + '", &seen); }')
    render = helper.block(source, "static void render_voice(")
    loop = helper.block(render, "for (int f = 0; f < 2; f++)")
    actual.append("static void filter_samples(Voice *v, float out[MAX_CH][FRAME]) {\n" + loop + "\n}")
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="audio-voice-biquad-") as temporary:
        folder = Path(temporary)
        (folder / "actual_audio_biquad.inc").write_text("\n\n".join(actual), encoding="utf-8")
        executable = folder / "fixture.exe"
        command = [args.cc, "-std=c11", "-include", ENV_ON_H, "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(folder),
                   str(ROOT / "tests/audio_voice_biquad_test.c"), "-lm", "-o", str(executable)]
        for command in (command, [str(executable)]):
            result = subprocess.run(command, creationflags=flags, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=30)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
