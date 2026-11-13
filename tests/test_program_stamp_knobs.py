"""Every switch the shader translation reads is in the program cache's stamp.

hookshot_programs.bin keeps translated programs; vk_shader.cpp translator_stamp hashes the environment variables whose names
start with one of its prefixes, so changing one translates the programs again. A switch that changes the translation but
is left out of the list does nothing for the programs already cached (RX_VK_NO_INVARIANT and the NaN guard were
such a case once; RX_VK_NO_CBUF_LOAD_RULE, RX_VK_PTR_SPLIT_FALLBACK and others were until this check). Read every
RX_VK_ name the translator (gpu/vk/eden/shader_recompiler) reads, and the translation's own switches in vk_shader.cpp,
and check each against the stamp's list. No game or GPU is needed.
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The translator's switches that only log: they change no program
LOG_ONLY = {
    "RX_VK_LOG_BRX": "logs the indirect branch tables it finds",
    "RX_VK_PTR_DIAG": "counts the pointer pass's outcomes for the log",
}
# The switches vk_shader.cpp reads that change what the translator is given
SHADER_SWITCHES = ("RX_VK_NO_DRAW_PARAMS", "RX_VK_ALL_GLOBAL_PROGS")


def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block: " + marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    shader = (args.source / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    stamp = block(shader, "uint64_t translator_stamp(")   # (disk_stamp before the program cache moved to vk_progcache.cpp)
    loop = stamp[stamp.index("_environ"):]
    prefixes = re.findall(r'"([A-Z0-9_]+)"', loop[:loop.index("add(*e")])
    if "EDEN_" not in prefixes:
        raise SystemExit("translator_stamp: the list of prefixes was not found")

    names = {}
    for path in sorted((args.source / "gpu" / "vk" / "eden" / "shader_recompiler").rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        for name in re.findall(r'(?:getenv|env_on)\("(RX_VK_[A-Z0-9_]+)"\)', path.read_text(encoding="utf-8", errors="replace")):
            names.setdefault(name, path.relative_to(args.source).as_posix())
    for name in SHADER_SWITCHES:
        if 'getenv("%s")' % name not in shader and 'env_on("%s")' % name not in shader:
            raise SystemExit("vk_shader.cpp no longer reads %s: update this check" % name)
        names.setdefault(name, "gpu/vk/vk_shader.cpp")
    if len(names) < 10:
        raise SystemExit("found only %d of the translator's switches: has it moved?" % len(names))

    missing = [(name, where) for name, where in sorted(names.items())
               if name not in LOG_ONLY and not any(name.startswith(p) for p in prefixes)]
    if missing:
        raise SystemExit("switches that change the translation but not the program cache's stamp (vk_shader.cpp translator_stamp):\n" +
                         "\n".join("  %s (%s)" % m for m in missing))
    # (nx2/bake_ds computes the same stamp offline: its copy of the list is the live one)
    bake = (args.source / "nx2" / "bake_ds" / "bake_ds.cpp").read_text(encoding="utf-8")
    bake_loop = bake[bake.index("for (char** e = environ;"):]
    bake_prefixes = re.findall(r'"([A-Z0-9_]+)"', bake_loop[:bake_loop.index("add(*e")])
    if bake_prefixes != prefixes:
        raise SystemExit("nx2/bake_ds/bake_ds.cpp's stamp list differs from vk_shader.cpp translator_stamp's:\n  bake_ds %s\n  live    %s"
                         % (bake_prefixes, prefixes))
    stale = [name for name in LOG_ONLY if name not in names]
    if stale:
        raise SystemExit("LOG_ONLY names a switch the translator no longer reads: " + ", ".join(stale))
    print("PASS: the %d switches the translation reads are all in the program cache's stamp (%d log only)" %
          (len(names) - len(LOG_ONLY), len(LOG_ONLY)))


if __name__ == "__main__":
    main()
