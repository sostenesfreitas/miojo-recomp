"""Bake skeleton end-to-end on synthetic data only (no game files, no GPU).

Builds a tiny HKSEED01 seed by hand (one graphics program, one compute
program), enumerates a synthetic BAHS blob through shader_census.programs_in,
then runs nx2/bake.py bake -> bank manifest, checking determinism, dedupe and
--resume idempotence. Run with: python3 -I -B tests/test_bake_skeleton.py
"""
import atexit
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_SKIP = {"windows": "its translate tests compile the whole recompiler (~164 files): over the runner's 600 s on Windows; Linux runs it"}
sys.path.insert(0, str(ROOT / "nx2"))
import shader_census as sc  # noqa: E402
import bake  # noqa: E402

CONTROL_SIZE = 0xD8


def control(stage, end=0x100):
    c = bytearray(CONTROL_SIZE)
    c[0:4] = sc.CONTROL_MAGIC
    struct.pack_into("<I", c, 0x38, 0x40)
    struct.pack_into("<II", c, 0x40, end, end)
    struct.pack_into("<H", c, 0x56, stage)
    return bytes(c)


def code(seed, end=0x100):
    return bytes((seed + i) & 0xFF for i in range(end))


def synthetic_seed(path):
    """HKSEED01 with 2 programs: [vertex, fragment] and [compute]."""
    stages = [(0x1111, 0, control(0), code(1)),
              (0x2222, 1, control(1), code(2)),
              (0x3333, 5, control(5), code(3))]
    programs = [(0, 1), (2,)]
    with open(path, "wb") as f:
        f.write(b"HKSEED01" + struct.pack("<II", len(programs), len(stages)))
        for key, stage, ctl, cs in stages:
            f.write(struct.pack("<QIII", key, stage, len(ctl), len(cs))
                    + ctl + cs)
        for ids in programs:
            f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))


def synthetic_bahs():
    base, blob, size = 0x100, 0x2000, 0x3000
    data = bytearray(size)
    data[0:4] = b"BAHS"
    struct.pack_into("<III", data, 0x14, base, size - blob, blob)
    stages = [(0, 0x1000, 0x000), (1, 0x1100, 0x100), (5, 0x1200, 0x200)]
    at = base + 0x158
    for k, (stage, ctl, off) in enumerate(stages):
        struct.pack_into("<IIIIQIi", data, at, 0x30, (0, 1, 3)[k], 0, 0,
                         off, 0xE0, ctl - (at + 16))
        data[ctl:ctl + CONTROL_SIZE] = control(stage)
        data[blob + off:blob + off + 0x100] = code(k)
        at += 0x30
    table = at
    struct.pack_into("<II", data, base + 0x150, table - (base + 0x150),
                     len(stages))
    struct.pack_into("<II", data, table, 0x30, 2)
    struct.pack_into("<IIII", data, table + 8, 0x14, 0, 0b11, 0)
    struct.pack_into("<IIII", data, table + 0x1C, 0x14, 0, 0b1, 2)
    return bytes(data)


def require_toolchain(case):
    """Skip (not fail) the translate/verify tests where there is no compiler
    or SPIR-V headers, the way tests/test_bake_mimo.py skips (run_all.py
    treats a missing tool the same way)."""
    sys.path.insert(0, str(ROOT / "nx2"))
    import bake_mimo  # noqa: E402
    try:
        bake_mimo.cxx_std(bake_mimo.cxx())   # (a compiler, and one with a C++23 mode: the recompiler needs it)
    except SystemExit as e:
        case.skipTest("no usable C++ compiler: %s" % e)
    if not bake_mimo.spirv_headers():
        case.skipTest("no SPIR-V headers (HOOKSHOT_SPIRV_HEADERS or a CPM "
                       "cache): only the miner could run")


_BUILT = None   # (gpu/shaderbatch as the first translate built it: obj/, the flags and the executable)


def translate(tmp, seed, bank, **kw):
    """bake.translate_seed with its tool folder in tmp. The tool (the vendored recompiler, ~164 files) is built by
    the first test that translates and copied, with its file times, into the others' folders, where build_tool finds
    it current: one build for this file instead of one per test. Each test's translation is still its own."""
    global _BUILT
    tool = os.path.join(tmp, "sb-tool-cache")
    no_output = shutil.ignore_patterns("translate")
    if _BUILT:
        shutil.copytree(_BUILT, tool, ignore=no_output)
    else:
        os.makedirs(tool)
    out = bake.translate_seed(seed, bank, tool_dir=tool, **kw)
    if not _BUILT:
        _BUILT = os.path.join(tempfile.mkdtemp(prefix="bake-sb-built-"), "tool")
        atexit.register(shutil.rmtree, os.path.dirname(_BUILT), True)
        shutil.copytree(tool, _BUILT, ignore=no_output)
    return out


class BakeSkeletonTests(unittest.TestCase):
    def test_enumeration_finds_both_programs(self):
        found = [[s[0] for s in p] for p in sc.programs_in(synthetic_bahs(), {})]
        self.assertEqual(found, [[0, 1], [5]])

    def test_code_key_matches_cxx_fnv(self):
        ctl, area = control(0), code(7)
        h = 0xCBF29CE484222325
        for b in ctl[:CONTROL_SIZE]:
            h = ((h ^ b) * 0x100000001B3) & ((1 << 64) - 1)
        for b in area:
            h = ((h ^ b) * 0x100000001B3) & ((1 << 64) - 1)
        self.assertEqual(bake.code_key(ctl, area), h)

    def test_bake_end_to_end(self):
        with tempfile.TemporaryDirectory(prefix="bake-skel-") as tmp:
            seed = os.path.join(tmp, "hookshot_shaders.bin")
            synthetic_seed(seed)
            bank = os.path.join(tmp, "bank")
            path, stats = bake.bake_seed(seed, bank)
            self.assertEqual(stats["programs"], 2)
            self.assertEqual(stats["stages"], 3)
            # graphics: 3 input variants x 2 sample states; compute: baseline
            self.assertEqual(stats["combos"], 7)
            with open(path, encoding="utf-8") as f:
                manifest = json.load(f)
            self.assertEqual(manifest["status"], "PENDING_GPU")
            self.assertEqual(len(manifest["stamp"]), 16)
            kinds = {c["kind"] for c in manifest["combos"]}
            self.assertEqual(kinds, {"graphics", "compute"})
            pipes = [c["pipeline"] for c in manifest["combos"] if c["pipeline"]]
            self.assertEqual(len(pipes), len(set(pipes)), "pipelines deduped")
            # resume is idempotent: same keys, same file
            # resume is idempotent: same keys, same file
            with open(path, encoding="utf-8") as f:
                before = json.load(f)
            path2, stats2 = bake.bake_seed(seed, bank, resume=True)
            with open(path2, encoding="utf-8") as f:
                after = json.load(f)
            self.assertEqual(before["combos"], after["combos"])
            self.assertEqual(stats2["combos"], 0)  # all skipped, none new
            # limit flag bounds the work
            _, limited = bake.bake_seed(seed, os.path.join(tmp, "bank2"),
                                        limit=1)
            self.assertEqual(limited["programs"], 1)

    def test_stamp_tracks_translator(self):
        self.assertEqual(bake.translator_stamp(), bake.translator_stamp())
        self.assertRegex(bake.translator_stamp(), r"^[0-9a-f]{16}$")

    def test_cli_bake(self):
        with tempfile.TemporaryDirectory(prefix="bake-cli-") as tmp:
            seed = os.path.join(tmp, "hookshot_shaders.bin")
            synthetic_seed(seed)
            bank = os.path.join(tmp, "bank")
            r = subprocess.run(
                [sys.executable, "-I", "-B", str(ROOT / "nx2" / "bake.py"),
                 "bake", "--seed", seed, "--bank", bank],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("@@done bake", r.stdout)
            self.assertTrue(os.path.isfile(os.path.join(bank, "bake_bank.json")))


class BakeTranslateTests(unittest.TestCase):
    """gpu/shaderbatch (adopted bake-mimo c60fd1d) over a seed with real
    Ampere EXIT stages (fixture shape credited to tests/test_bake_mimo.py,
    bake-mimo): exact runtime keys + SPIR-V out, spirv-val vulkan1.3 VALID."""

    EXIT = struct.pack("<4I", 0x0000794D, 0, 0x03800000, 0x000FEA00)

    def real_control(self, stage, code_len, workgroup=(8, 1, 1)):
        c = bytearray(CONTROL_SIZE)
        c[0:4] = sc.CONTROL_MAGIC
        struct.pack_into("<I", c, 0x38, code_len)
        struct.pack_into("<II", c, 0x40, code_len, code_len)
        struct.pack_into("<H", c, 0x56, stage)
        if stage == 5:
            struct.pack_into("<3I", c, 0x58, *workgroup)
        return bytes(c)

    def real_header(self, shader_type):
        h = bytearray(128)
        struct.pack_into("<I", h, 0, shader_type << 10)
        return bytes(h)

    def real_seed(self, path):
        cs = (self.real_control(5, len(self.EXIT)), self.EXIT)
        vs = (self.real_control(0, 128 + 2 * len(self.EXIT)),
              self.real_header(1) + self.EXIT + self.EXIT)
        fs = (self.real_control(1, 128 + 2 * len(self.EXIT)),
              self.real_header(5) + self.EXIT + self.EXIT)
        stages = [(0xA001, 0, *vs), (0xA002, 1, *fs), (0xA003, 5, *cs)]
        programs = [(0, 1), (2,)]
        with open(path, "wb") as f:
            f.write(b"HKSEED01" + struct.pack("<II", len(programs),
                                             len(stages)))
            for key, stage, ctl, code in stages:
                f.write(struct.pack("<QIII", key, stage, len(ctl), len(code))
                        + ctl + code)
            for ids in programs:
                f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))

    def test_translate_exact_keys_and_spirv(self):
        require_toolchain(self)
        with tempfile.TemporaryDirectory(prefix="bake-sb-") as tmp:
            seed = os.path.join(tmp, "hookshot_shaders.bin")
            self.real_seed(seed)
            bank = os.path.join(tmp, "bank")
            bake.bake_seed(seed, bank)
            tr, rows = translate(tmp, seed, bank)
            self.assertTrue(rows, "no program rows from shaderbatch")
            for (idx, inputs), r in sorted(rows.items()):
                self.assertRegex(r["runtime_key"], r"^[0-9a-f]{16}$", r)
            ok = [r for r in rows.values() if r["status"] == "OK"]
            self.assertTrue(ok, [r["status"] for r in rows.values()])
            spvs = [os.path.join(tr, s)
                    for r in rows.values() for s in r["spv"]]
            self.assertTrue(spvs, "no SPIR-V files listed")
            for m in spvs:
                with open(m, "rb") as f:
                    self.assertEqual(f.read(4), b"\x03\x02\x23\x07", m)
            if shutil.which("spirv-val"):
                for m in spvs:
                    r = subprocess.run(
                        ["spirv-val", "--target-env", "vulkan1.3", m],
                        capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, (m, r.stdout, r.stderr))
            with open(os.path.join(bank, "bake_bank.json"),
                      encoding="utf-8") as f:
                manifest = json.load(f)
            self.assertEqual(manifest["translation"]["rows"], len(rows))
            self.assertGreater(
                manifest["translation"]["translated"], 0)


class BakeStampTrapTests(unittest.TestCase):
    """docs/BAKE.md s6 trap 1: build.py hashes sorted(translator) + the three
    named files APPENDED, not merged (nx2/translator_files.py). My translator_stamp must follow the
    appended order; this fails if someone 'simplifies' it into one sort."""

    def test_stamp_uses_appended_order(self):
        import hashlib
        vk = os.path.join(ROOT, "gpu", "vk")
        translator = [os.path.join(d, f) for top in ("eden", "sirit", "compat")
                      for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
        named = [os.path.join(vk, n) for n in
                 ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h")]   # (nx2/translator_files.py FILES)

        def stamp(order):
            h = hashlib.sha1()
            for f in order:
                with open(f, "rb") as src:
                    h.update(os.path.relpath(f, vk).replace("\\", "/").encode()
                             + src.read().replace(bytes((13, 10)), bytes((10,))))
            return h.hexdigest()[:16]
        self.assertNotEqual(bake.translator_stamp(), stamp(sorted(translator + named)),   # the WRONG order (trap)
                            "stamp matches the fully-sorted order: s6.4 trap")
        self.assertEqual(bake.translator_stamp(), stamp(sorted(translator) + named),
                         "stamp is not the folders' files sorted, then the named files appended")
        self.assertRegex(bake.translator_stamp(), r"^[0-9a-f]{16}$")


class BakeVerifyTests(unittest.TestCase):
    """bake verify over a translated bank: seed round-trip, stamp currency,
    SPIR-V magic + spirv-val over every merged module."""

    def test_verify_bank(self):
        require_toolchain(self)
        t = BakeTranslateTests()
        with tempfile.TemporaryDirectory(prefix="bake-verify-") as tmp:
            seed = os.path.join(tmp, "hookshot_shaders.bin")
            t.real_seed(seed)
            bank = os.path.join(tmp, "bank")
            bake.bake_seed(seed, bank)
            translate(tmp, seed, bank)
            summary, ok = bake.verify_bank(bank)
            self.assertTrue(summary["manifest"])
            self.assertTrue(summary["seed_ok"])
            self.assertTrue(summary["stamp_current"])
            self.assertGreater(summary["translation_rows"], 0)
            self.assertGreater(summary["spv_files"], 0)
            self.assertEqual(summary["spv_bad_magic"], 0)
            # the gate is the shipping layout (uniformBufferStandardLayout,
            # vk_main.cpp:253,256,360), not strict validity
            self.assertEqual(summary["spirv_layout"],
                             "uniformBufferStandardLayout")
            self.assertEqual(summary["spirv_invalid"], 0)
            # full coverage on the synthetic seed
            self.assertEqual(summary["seed_programs"], 2)
            self.assertEqual(summary["coverage_programs"], 2)
            self.assertEqual(summary["missing_programs"], 0)
            self.assertTrue(ok, summary)
            self.assertTrue(
                os.path.isfile(os.path.join(bank, "verify.json")))
            r = subprocess.run(
                [sys.executable, "-I", "-B", str(ROOT / "nx2" / "bake.py"),
                 "verify", "--bank", bank],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("@@done verify", r.stdout)

    def test_verify_partial_bank_stays_ok(self):
        """ROM-STATUS scale-down: a bank missing a program (LOWER_ERROR never
        enters the bank) still verifies ok -- the game translates the missing
        program live at first meet (program_pick, vk_shader.cpp:1685-1700)."""
        require_toolchain(self)
        t = BakeTranslateTests()
        with tempfile.TemporaryDirectory(prefix="bake-partial-") as tmp:
            seed = os.path.join(tmp, "hookshot_shaders.bin")
            t.real_seed(seed)
            bank = os.path.join(tmp, "bank")
            bake.bake_seed(seed, bank)
            translate(tmp, seed, bank, limit=1)
            summary, ok = bake.verify_bank(bank)
            self.assertEqual(summary["seed_programs"], 2)
            self.assertEqual(summary["coverage_programs"], 1)
            self.assertEqual(summary["missing_programs"], 1)
            self.assertAlmostEqual(summary["coverage_fraction"], 0.5)
            self.assertEqual(summary["spirv_invalid"], 0)
            self.assertTrue(ok, summary)


if __name__ == "__main__":
    unittest.main(verbosity=1)
