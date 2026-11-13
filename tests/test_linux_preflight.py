"""Linux source prerequisites stop the real builder before extraction or translation.

All dependency answers and game bytes are authored. The production C probe is also
compiled (not linked) with --cc gcc or --cc clang: CI needs no game linker.
HOOKSHOT_LINUX_PREFLIGHT_SOURCE selects another checkout's nx2 sources for the
exact same fail-before checks.
"""
import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

RUN_ALL_ARGS = ["--cc", "clang"]

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("HOOKSHOT_LINUX_PREFLIGHT_SOURCE", ROOT))
COMPILER = None


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReachedGame(Exception):
    pass


class LinuxPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-linux-preflight-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.dict(os.environ, {}, clear=True))
        self.stack.enter_context(mock.patch.dict(sys.modules))
        self.stack.enter_context(mock.patch.object(sys, "path", list(sys.path)))
        # Import only production modules, without reading a developer's dump.txt or
        # probing the actual system's pkg-config at import time.
        os.environ["OOT_WORK"] = str(self.root / "work")
        for name in ("kit", "build", "toolchain"):
            sys.modules.pop(name, None)
        sys.path[:0] = [str(SOURCE / "nx2"), str(SOURCE / "third_party/a64-kit/tools")]
        self.processes = []
        self.pc_missing = set()
        self.compiler_error = ""
        self.real_compile = False
        self.runner = self.stack.enter_context(mock.patch.object(subprocess, "run", side_effect=self.run_process))
        self.make = load("linux_make_game", SOURCE / "nx2/make_game.py")
        self.build = load("build", SOURCE / "nx2/build.py")
        sys.modules["build"] = self.build
        self.processes.clear()
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        (self.checkout / "requirements.txt").write_bytes((ROOT / "requirements.txt").read_bytes())
        self.stack.enter_context(mock.patch.object(self.make, "ROOT", str(self.checkout)))
        self.stack.enter_context(mock.patch.object(self.make, "LINUX", True))
        self.stack.enter_context(mock.patch.object(self.make, "CROSS", False))
        self.stack.enter_context(mock.patch.object(self.build, "LINUX", True))
        self.stack.enter_context(mock.patch.object(self.build, "NGX_SDK", ""))
        self.stack.enter_context(mock.patch.object(self.build.kit, "ROOT", str(self.checkout)))
        self.stack.enter_context(mock.patch.object(self.build.kit, "WORK", str(self.root / "wrong-work")))
        self.cc = self.root / "tools/clang"
        self.cc.parent.mkdir()
        self.cc.write_text("authored compiler path\n")
        self.cxx = self.cc.with_name("clang++")
        self.cxx.write_text("authored C++ compiler path\n")
        self.find = self.stack.enter_context(mock.patch.object(self.build.toolchain, "find", side_effect=lambda n: str(self.cc if n == "clang" else self.cxx)))
        self.modules_missing = set()
        self.module_queries = []
        self.stack.enter_context(mock.patch.object(importlib.util, "find_spec", side_effect=self.find_module))
        self.system_headers = set()
        isfile = os.path.isfile

        def file_exists(path):
            name = os.fspath(path).replace(os.sep, "/")
            if name.startswith(("/usr/include/", "/usr/local/include/")):
                return name in self.system_headers
            return isfile(path)

        self.stack.enter_context(mock.patch.object(os.path, "isfile", side_effect=file_exists))
        for name in ("no_throttle", "check_cpu", "warn_slow_disk", "stop_stray_compilers", "drop_stale_objects"):
            self.stack.enter_context(mock.patch.object(self.make, name))
        self.stack.enter_context(mock.patch("ctypes.WinDLL", create=True))
        self.stack.enter_context(mock.patch.object(os, "system", side_effect=AssertionError("shell command")))
        self.stack.enter_context(mock.patch.object(os, "startfile", create=True, side_effect=AssertionError("file launch")))
        self.stack.enter_context(mock.patch.object(subprocess, "Popen", side_effect=AssertionError("unplanned process")))
        self.events = []
        self.stack.enter_context(mock.patch.object(self.make, "run", side_effect=self.game_step))
        self.stack.enter_context(mock.patch.object(self.make, "lift_key", return_value="authored lift key"))
        self.nup = self.root / "authored.nup"
        self.nup.write_bytes(b"authored package, never passed to a real extractor")
        self.output = io.StringIO()

    def find_module(self, name):
        self.module_queries.append(name)
        return None if name in self.modules_missing else object()

    def run_process(self, cmd, **kwargs):
        self.processes.append(cmd)
        if cmd[0] == "pkg-config":
            bad = self.pc_missing.intersection(cmd[2:])
            if bad and kwargs.get("check"):
                raise subprocess.CalledProcessError(1, cmd, stderr="authored missing pkg-config entry")
            return subprocess.CompletedProcess(cmd, bool(bad), "", "")
        if "-o" in cmd:
            if self.real_compile:
                return REAL_RUN([*cmd, "-c"], **kwargs)
            return subprocess.CompletedProcess(cmd, bool(self.compiler_error), "", self.compiler_error)
        self.fail("Unexpected command: %r" % cmd)

    def game_step(self, cmd, *args, **kwargs):
        if "nx2/nup.py" in cmd:
            self.events.append("read")
            dest = Path(cmd[cmd.index("--extract") + 1])
            (dest / "exefs").mkdir(parents=True)
            (dest / "romfs").mkdir()
            header = bytearray(0x60)
            header[:4] = b"NSO0"
            header[0x40:0x44] = bytes.fromhex(self.make.BUILD_ID)
            (dest / "exefs/main").write_bytes(header)
            return 0
        if "nx2/lift.py" in cmd:
            self.events.append("lift")
            raise ReachedGame
        self.fail("Unexpected game step: %r" % cmd)

    def invoke(self):
        argv = ["make_game.py", "--jobs", "1", "--nup", str(self.nup), "--out", str(self.root / "output"),
                "--work", str(self.root / "work"), "--no-prepare-shaders"]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(self.output):
            try:
                self.make.main()
            except ReachedGame:
                return "game"
            except SystemExit as e:
                return e.code
        self.fail("builder returned without the authored stop")

    def headers(self, mode="cpm"):
        packages = (("vulkanheaders", "vulkan/vulkan.h", "HOOKSHOT_VULKAN_HEADERS"),
                    ("spirv-headers", "spirv/unified1/spirv.h", "HOOKSHOT_SPIRV_HEADERS"),
                    ("vulkanmemoryallocator", "vk_mem_alloc.h", "HOOKSHOT_VMA_HEADERS"))
        roots = []
        for package, probe, env in packages:
            base = self.root / ("cache/" + package + "/001" if mode == "cpm" else "explicit/" + package)
            path = base / "include" / probe
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* authored header sentinel */\n")
            roots.append(str(base))
            if mode != "cpm":
                os.environ[env] = str(base / "include" if mode == "include" else base)
        if mode == "cpm":
            os.environ["RX_CPM"] = str(self.root / "cache")
        return roots

    def test_all_missing_reported_before_any_dump_or_lift(self):
        self.modules_missing = {"zstandard", "numpy"}
        self.pc_missing = {"libavcodec", "libswscale"}
        self.cc.unlink()
        self.cxx.unlink()
        result = self.invoke()
        text = self.output.getvalue()
        self.assertEqual((result, self.events), (1, []), "main reached game work before the preflight: " + repr(self.events))
        for name in ("zstandard", "numpy", "Vulkan", "SPIR-V", "VMA", "libavcodec", "libswscale", "clang", "clang++",
                     "isolated import path", "RX_CPM", "HOOKSHOT_VULKAN_HEADERS", "/usr/local", "pkg-config", "EMSDK", "PATH"):
            self.assertIn(name, text)
        self.assertEqual(text.count("@@error"), 1)
        self.assertNotIn("@@stage read", text)
        self.assertFalse((self.root / "work/exefs").exists())
        self.assertFalse((self.root / "work/sa").exists())

    def test_injected_toolchain_check_keeps_game_free_steps_hermetic(self):
        # Cache/missing-input fixtures inject this same boundary along with run().
        # A future exception in their stand-in lift is not a production build mode.
        self.modules_missing = {"lz4", "numpy", "capstone", "unicorn", "zstandard"}
        self.pc_missing = {"libavcodec", "libswscale", "libswresample", "libavutil"}
        self.cc.unlink()
        self.cxx.unlink()
        with mock.patch.object(self.make, "check_windows_sdk") as preflight:
            self.assertEqual(self.invoke(), "game", self.output.getvalue())
        preflight.assert_called_once_with(str(self.root / "work"))
        self.assertEqual(self.events, ["read", "lift"])
        self.assertEqual(self.module_queries, [])
        self.assertEqual(self.processes, [])

    def test_valid_cpm_reaches_read_then_lift(self):
        expected = self.headers()
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertEqual(self.events, ["read", "lift"])
        self.assertEqual(self.build.vulkan_headers(), expected)
        self.assertIn("Linux build prerequisites: OK", self.output.getvalue())

    def test_explicit_package_roots(self):
        expected = self.headers("root")
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertEqual(self.build.vulkan_headers(), expected)

    def test_explicit_include_roots(self):
        expected = self.headers("include")
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertEqual(self.build.vulkan_headers(), expected)

    def test_cpm_wins_over_explicit_and_system(self):
        self.headers("include")
        expected = self.headers()
        self.system_headers = {"/usr/include/vulkan/vulkan.h", "/usr/include/spirv/unified1/spirv.h", "/usr/include/vk_mem_alloc.h"}
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertEqual(self.build.vulkan_headers(), expected)

    def test_system_prefixes_use_the_build_resolver(self):
        for prefix in ("/usr/local", "/usr"):
            with self.subTest(prefix=prefix):
                self.system_headers = {prefix + "/include/" + h for h in ("vulkan/vulkan.h", "spirv/unified1/spirv.h", "vk_mem_alloc.h")}
                self.assertEqual(self.build.vulkan_headers(), [prefix] * 3)
                self.make.check_linux_prerequisites()

    def test_selected_work_cache_not_checkouts_default(self):
        self.headers()
        shutil.move(str(self.root / "cache"), str(self.root / "work-cache"))
        dest = self.root / "work/eg/.cache"
        dest.mkdir(parents=True)
        shutil.move(str(self.root / "work-cache"), str(dest / "cpm"))
        os.environ.pop("RX_CPM")
        self.assertEqual(self.invoke(), "game", self.output.getvalue())

    def test_prebuilt_renderer_needs_no_renderer_headers(self):
        renderer = self.root / "prebuilt.so"
        renderer.write_bytes(b"authored renderer path")
        os.environ["HOOKSHOT_VK_PREBUILT"] = str(renderer)
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertIn(mock.call("clang++"), self.find.call_args_list)   # a64emit still builds from source

    def test_source_prebuilt_still_needs_native_cpp_compiler(self):
        renderer = self.root / "prebuilt.so"
        renderer.write_bytes(b"authored renderer path")
        os.environ["HOOKSHOT_VK_PREBUILT"] = str(renderer)
        self.cxx.unlink()
        self.assertEqual(self.invoke(), 1)
        self.assertIn("clang++", self.output.getvalue())
        self.assertNotIn("VMA", self.output.getvalue())
        self.assertEqual(self.events, [])

    def test_ngx_overrides_prebuilt_renderer(self):
        os.environ["HOOKSHOT_VK_PREBUILT"] = str(self.root / "ignored.so")
        self.build.NGX_SDK = str(self.root / "sdk")
        self.assertEqual(self.invoke(), 1)
        self.assertIn("SPIR-V", self.output.getvalue())
        self.assertNotIn("ignored.so", self.output.getvalue())
        self.assertEqual(self.events, [])

    def test_missing_optional_prompt_packages_are_never_required(self):
        self.headers()
        self.modules_missing = {"fontTools", "PIL", "astc_encoder", "texture2ddecoder"}
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertEqual(set(self.module_queries), {"lz4", "numpy", "capstone", "unicorn", "zstandard"})

    def test_bundled_static_ffmpeg_avoids_system_pkg_config(self):
        (self.checkout / "requirements.txt").unlink()  # AppImage packaging carries Python, not requirements.txt.
        self.cxx.unlink()   # the bundle carries a64emit too; no renderer or helper C++ compile
        renderer = self.root / "prebuilt.so"
        renderer.write_bytes(b"authored renderer path")
        os.environ["HOOKSHOT_VK_PREBUILT"] = str(renderer)
        prefix = self.root / "ffmpeg"
        os.environ["HOOKSHOT_FFMPEG"] = str(prefix)
        for lib in self.build.FFMPEG_LIBS:
            for rel in ("lib/lib" + lib + ".a", "include/lib" + lib + "/" + lib + ".h"):
                path = prefix / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"authored static FFmpeg path")
        self.pc_missing = {"libavcodec", "libswscale", "libswresample", "libavutil"}
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertFalse(any(cmd[0] == "pkg-config" for cmd in self.processes))
        self.assertEqual(self.module_queries, [])

    def test_partial_static_prefix_reports_each_missing_file(self):
        self.headers()
        prefix = self.root / "ffmpeg"
        (prefix / "lib").mkdir(parents=True)
        (prefix / "lib/libavcodec.a").write_bytes(b"authored archive sentinel")
        os.environ["HOOKSHOT_FFMPEG"] = str(prefix)
        self.assertEqual(self.invoke(), 1)
        text = self.output.getvalue()
        for name in ("libswscale.a", "libswresample.a", "libavutil.a", "avcodec.h", "swscale.h", "swresample.h", "avutil.h"):
            self.assertIn(name, text)
        self.assertEqual(self.events, [])

    def test_linker_failure_keeps_diagnostic_and_other_missing_items(self):
        self.compiler_error = "clang: error: invalid linker name in argument '-fuse-ld=lld'\n"
        self.modules_missing = {"capstone"}
        self.assertEqual(self.invoke(), 1)
        text = self.output.getvalue()
        for name in ("capstone", "VMA", "invalid linker name", "HOOKSHOT_SYSROOT"):
            self.assertIn(name, text)
        self.assertEqual(self.events, [])

    def test_cross_build_uses_windows_toolchain_not_linux_packages(self):
        self.make.LINUX, self.make.CROSS = False, True
        os.environ["RX_TARGET"] = "windows"
        os.environ["HOOKSHOT_MINGW"] = str(self.root / "mingw")
        self.pc_missing = {"libavcodec"}
        self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertFalse(any(cmd[0] == "pkg-config" for cmd in self.processes))
        self.assertTrue(any("--target=x86_64-w64-mingw32" in cmd for cmd in self.processes))
        self.assertEqual(self.events, ["read", "lift"])

    @unittest.skipIf(os.name == "nt", "real Linux C compile; authored routing cases run on Windows too")
    def test_real_compiler_compiles_production_probe_without_a_linker(self):
        self.headers()
        self.cc = Path(COMPILER)
        self.real_compile = True
        # The command still asks for lld, but compile-only needs no linker installed.
        with mock.patch.object(subprocess, "Popen", REAL_POPEN), mock.patch.dict(os.environ, {"PATH": REAL_PATH}):
            self.assertEqual(self.invoke(), "game", self.output.getvalue())
        self.assertTrue(any("-fuse-ld=lld" in cmd for cmd in self.processes))


REAL_RUN, REAL_POPEN = subprocess.run, subprocess.Popen
REAL_PATH = os.environ.get("PATH", "")
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args, remaining = parser.parse_known_args()
    COMPILER = shutil.which(args.cc) or args.cc
    unittest.main(argv=[sys.argv[0], *remaining])
