"""The game's CPU target follows the PC that builds it, and the AppImage says up front when its renderer cannot run
(nx2/host_cpu.py, nx2/make_game.py's check_cpu; issue 363). Game-free.

python -I -B tests/test_cpu_target.py
"""
import builtins
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


host_cpu = load("host_cpu_under_test", ROOT / "nx2" / "host_cpu.py")

# (/proc/cpuinfo flags, cut to what matters plus some noise)
HASWELL = "fpu sse sse2 pni ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt xsave avx f16c lahf_lm abm bmi1 avx2 bmi2"
IVY_BRIDGE = "fpu sse sse2 pni ssse3 cx16 sse4_1 sse4_2 popcnt xsave avx f16c lahf_lm"   # (the Xeon E5-2667 v2 of 363)
CORE2 = "fpu sse sse2 pni ssse3 cx16 lahf_lm"


def cpuinfo(flags):
    return "processor\t: 0\nvendor_id\t: GenuineIntel\nflags\t\t: %s\nvmx flags\t: ept\n\nprocessor\t: 1\n" \
           "flags\t\t: %s\n" % (flags, flags)


def march_on_linux(text):
    real_open = builtins.open

    def fake_open(path, *args, **kwargs):
        if path == "/proc/cpuinfo":
            return io.StringIO(text)
        return real_open(path, *args, **kwargs)
    with mock.patch.object(host_cpu.os, "name", "posix"), mock.patch.object(builtins, "open", fake_open):
        return host_cpu.host_march()


class HostMarch(unittest.TestCase):
    def test_levels_from_cpuinfo(self):
        self.assertEqual(march_on_linux(cpuinfo(HASWELL)), "x86-64-v3")
        self.assertEqual(march_on_linux(cpuinfo(IVY_BRIDGE)), "x86-64-v2")
        self.assertEqual(march_on_linux(cpuinfo(CORE2)), "x86-64")

    def test_unreadable_cpu_keeps_v3(self):
        self.assertEqual(march_on_linux("processor\t: 0\n"), "x86-64-v3")

    def test_steamdeck_mtune(self):
        real_open = builtins.open

        def fake_deck(path, *args, **kwargs):
            if path == "/proc/cpuinfo":
                return io.StringIO("model name\t: AMD Custom APU 0405\nflags\t\t: fpu\n")
            return real_open(path, *args, **kwargs)

        with mock.patch.object(host_cpu.os, "name", "posix"), mock.patch.object(builtins, "open", fake_deck), \
                mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(host_cpu.host_mtune(), "znver2")

        with mock.patch.dict(os.environ, {"RX_STEAMDECK": "1"}, clear=True):
            self.assertEqual(host_cpu.host_mtune(), "znver2")

    def test_windows(self):
        class K32:
            def __init__(self, present):
                self.IsProcessorFeaturePresent = lambda pf: pf in present
        for present, want in (({38, 40}, "x86-64-v3"), ({38}, "x86-64-v2"), (set(), "x86-64")):
            ctypes = mock.MagicMock()
            ctypes.windll.kernel32 = K32(present)
            with mock.patch.object(host_cpu.os, "name", "nt"), mock.patch.dict(sys.modules, {"ctypes": ctypes}):
                self.assertEqual(host_cpu.host_march(), want, present)

    def test_build_and_appimage_use_it(self):
        build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        self.assertIn("from host_cpu import host_march", build)
        self.assertIn('MARCH = os.environ.get("RX_MARCH") or host_march()', build)
        appimage = (ROOT / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8")
        self.assertIn("RX_MARCH=x86-64-v3 RX_VK_ONLY=1", appimage)   # (its prebuilt renderer: not the CI runner's CPU)

    def test_build_message_names_the_cause(self):
        """'this CPU has no AVX2' only when the CPU chose the level: an RX_MARCH of znver2 (AVX2 and more) said it too"""
        build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        start = build.index('MARCH = os.environ.get("RX_MARCH") or host_march()')
        block = build[start:build.index("CPU = [", start)]
        for env, host, want in (({"RX_MARCH": "znver2"}, "x86-64-v3", "built for znver2 (RX_MARCH)"),
                                ({"RX_MARCH": "x86-64-v3"}, "x86-64", ""),
                                ({}, "x86-64-v2", "this CPU has no AVX2"),
                                ({}, "x86-64-v3", "")):
            out = io.StringIO()
            with mock.patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(out):
                exec(block, {"os": os, "host_march": lambda h=host: h})
            if want:
                self.assertIn(want, out.getvalue(), env)
            else:
                self.assertEqual(out.getvalue(), "", env)
            if env:
                self.assertNotIn("no AVX2", out.getvalue(), env)


class AppImageCheck(unittest.TestCase):
    """make_game.py stops before reading the game when the AppImage's x86-64-v3 renderer meets a CPU without AVX2"""

    def setUp(self):
        self.make_game = load("make_game_cpu_test", ROOT / "nx2" / "make_game.py")

    def run_check(self, env, march):
        fake = mock.MagicMock(host_march=mock.MagicMock(return_value=march))
        with mock.patch.dict(os.environ, {}, clear=False), mock.patch.dict(sys.modules, {"host_cpu": fake}), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            for k in ("HOOKSHOT_VK_PREBUILT", "RX_MARCH"):
                os.environ.pop(k, None)
            os.environ.update(env)
            try:
                self.make_game.check_cpu()
            except SystemExit as stopped:
                return stopped.code, out.getvalue()
        return None, out.getvalue()

    def test_appimage_without_avx2_stops(self):
        code, out = self.run_check({"HOOKSHOT_VK_PREBUILT": "/x/hookshot_vk.so"}, "x86-64-v2")
        self.assertEqual(code, 1)
        self.assertIn("AVX2", out)

    def test_appimage_with_avx2_goes_on(self):
        self.assertIsNone(self.run_check({"HOOKSHOT_VK_PREBUILT": "/x/hookshot_vk.so"}, "x86-64-v3")[0])

    def test_source_build_without_avx2_goes_on(self):
        self.assertIsNone(self.run_check({}, "x86-64-v2")[0])

    def test_rx_march_is_the_players_choice(self):
        env = {"HOOKSHOT_VK_PREBUILT": "/x/hookshot_vk.so", "RX_MARCH": "x86-64-v2"}
        self.assertIsNone(self.run_check(env, "x86-64-v2")[0])

    def test_checked_before_the_game_is_read(self):
        src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
        main = src[src.index("def main("):]
        self.assertLess(main.index("check_cpu()"), main.index("drop_stale_objects(work)"))
        check = src[src.index("def check(a, work):"):]
        self.assertLess(check.index("check_cpu()"), check.index("if a.nup:"))


if __name__ == "__main__":
    unittest.main()
