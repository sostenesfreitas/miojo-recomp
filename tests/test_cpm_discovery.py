"""Discover authored CLI package layouts; no real dump, compiler or dependency headers."""
import contextlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "build.py"


class CpmDiscoveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-cpm-fixture-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.tools = self.base / "portable builder \u03a9" / "tools"
        self.tools.mkdir(parents=True)
        self.bundled = self.tools / "cpm"
        self.legacy = self.tools / "work" / "eg" / ".cache" / "cpm"

    def cache(self, root):
        ffmpeg = root / "ffmpeg" / "authored-version"
        (ffmpeg / "lib").mkdir(parents=True)
        header = ffmpeg / "include" / "libavcodec" / "avcodec.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        vulkan = root / "vulkanheaders" / "authored-version"
        header = vulkan / "include" / "vulkan" / "vulkan.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        spirv = root / "spirv-headers" / "authored-version"
        header = spirv / "include" / "spirv" / "unified1" / "spirv.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        vma = root / "vulkanmemoryallocator" / "authored-version"
        header = vma / "include" / "vk_mem_alloc.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        return {"ffmpeg": str(ffmpeg), "vulkanheaders": str(vulkan),
                "spirv-headers": str(spirv), "vulkanmemoryallocator": str(vma)}

    @contextlib.contextmanager
    def build(self, override="", extra_env=None):
        # The real kit reads dump settings on import. Replace only unrelated imports
        # so the real build module discovers packages in this owned layout instead.
        kit = types.SimpleNamespace(ROOT=str(self.tools), HERE=str(self.tools / "nx2"),
                                    WORK=str(self.tools / "work"))
        toolchain = types.SimpleNamespace(windows=lambda: os.name == "nt")

        def no_process(command, *args, **kwargs):
            if command[0] == "pkg-config":
                raise OSError("This fixture has no system FFmpeg")
            raise AssertionError("Unexpected build process: " + repr(command))

        env = {"RX_CPM": str(override), "HOOKSHOT_FFMPEG": "",
               "HOOKSHOT_VULKAN_HEADERS": "", "HOOKSHOT_SPIRV_HEADERS": "", "HOOKSHOT_VMA_HEADERS": ""}
        if extra_env:
            env.update(extra_env)
        with (mock.patch.dict(sys.modules, {"kit": kit, "rtgen": types.ModuleType("rtgen"),
                                           "toolchain": toolchain}),
              mock.patch.object(sys, "path", list(sys.path)),
              mock.patch.dict(os.environ, env, clear=False),
              mock.patch.object(subprocess, "run", side_effect=no_process),
              mock.patch.object(subprocess, "Popen", side_effect=AssertionError("Unexpected process launch"))):
            spec = importlib.util.spec_from_file_location("cpm_build_fixture", SOURCE)
            build = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(build)
            yield build

    def assert_packages(self, build, expected):
        self.assertEqual(build.cpm_package("ffmpeg"), expected["ffmpeg"],
                         "bundled FFmpeg was not discovered")
        self.assertEqual(build.cpm_package("vulkanheaders"), expected["vulkanheaders"],
                         "bundled Vulkan headers were not discovered")
        self.assertEqual(build.header_package("vulkanheaders", "vulkan/vulkan.h", "HOOKSHOT_VULKAN_HEADERS"),
                         expected["vulkanheaders"])
        self.assertEqual(build.header_package("spirv-headers", "spirv/unified1/spirv.h", "HOOKSHOT_SPIRV_HEADERS"),
                         expected["spirv-headers"])
        self.assertEqual(build.header_package("vulkanmemoryallocator", "vk_mem_alloc.h", "HOOKSHOT_VMA_HEADERS"),
                         expected["vulkanmemoryallocator"])
        if not build.LINUX:
            self.assertEqual(build.FFMPEG, expected["ffmpeg"], "Windows import did not select FFmpeg")

    def test_portable_package_without_rx_cpm(self):
        expected = self.cache(self.bundled)
        with self.build() as build:
            self.assert_packages(build, expected)

    def test_explicit_override_wins(self):
        self.cache(self.bundled)
        self.cache(self.legacy)
        override = self.base / "explicit cache"
        expected = self.cache(override)
        with self.build(override) as build:
            self.assert_packages(build, expected)

    def test_bundle_precedes_legacy_cache(self):
        expected = self.cache(self.bundled)
        self.cache(self.legacy)
        with self.build() as build:
            self.assert_packages(build, expected)

    def test_legacy_fallback_without_bundle(self):
        expected = self.cache(self.legacy)
        with self.build() as build:
            self.assert_packages(build, expected)

    def test_env_include_dir_as_package_root(self):
        # HOOKSHOT_*_HEADERS matches tests/run_all.py: the include/ folder itself.
        sysroot = self.base / "sysroot"
        for rel in ("include/vulkan/vulkan.h", "include/spirv/unified1/spirv.h", "include/vk_mem_alloc.h"):
            path = sysroot / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* sys */\n", encoding="utf-8")
        with self.build(extra_env={
            "HOOKSHOT_VULKAN_HEADERS": str(sysroot / "include"),
            "HOOKSHOT_SPIRV_HEADERS": str(sysroot / "include"),
            "HOOKSHOT_VMA_HEADERS": str(sysroot / "include"),
        }) as build:
            self.assertEqual(build.header_package("vulkanheaders", "vulkan/vulkan.h", "HOOKSHOT_VULKAN_HEADERS"),
                             str(sysroot))
            self.assertEqual(build.header_package("spirv-headers", "spirv/unified1/spirv.h", "HOOKSHOT_SPIRV_HEADERS"),
                             str(sysroot))
            self.assertEqual(build.header_package("vulkanmemoryallocator", "vk_mem_alloc.h", "HOOKSHOT_VMA_HEADERS"),
                             str(sysroot))

    def test_system_usr_without_cpm(self):
        if not (Path("/usr/include/vulkan/vulkan.h").is_file() and
                Path("/usr/include/spirv/unified1/spirv.h").is_file()):
            self.skipTest("no system vulkan-headers / spirv-headers")
        with self.build() as build:
            self.assertEqual(
                build.header_package("vulkanheaders", "vulkan/vulkan.h", "HOOKSHOT_VULKAN_HEADERS"), "/usr")
            self.assertEqual(
                build.header_package("spirv-headers", "spirv/unified1/spirv.h", "HOOKSHOT_SPIRV_HEADERS"), "/usr")
            if Path("/usr/include/vk_mem_alloc.h").is_file():
                self.assertEqual(
                    build.header_package("vulkanmemoryallocator", "vk_mem_alloc.h", "HOOKSHOT_VMA_HEADERS"), "/usr")
            else:
                self.assertIsNone(
                    build.header_package("vulkanmemoryallocator", "vk_mem_alloc.h", "HOOKSHOT_VMA_HEADERS"))

    def test_dav1d_windows_only_with_llvm_mingw(self):
        """dav1d (scripts/windows/dav1d.sh) is linked for the AV1 movies on Windows with llvm-mingw only, and only whole"""
        self.cache(self.bundled)
        dav1d = self.bundled / "dav1d" / "1.5.4"
        header, lib = dav1d / "include" / "dav1d" / "dav1d.h", dav1d / "lib" / "libdav1d.a"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        lib.parent.mkdir(parents=True)
        lib.write_bytes(b"!<arch>\n")
        with self.build() as build:
            build.LINUX = False
            build.toolchain = types.SimpleNamespace(mingw=lambda: "llvm-mingw")
            self.assertEqual(build.dav1d_windows(), str(dav1d))
            build.toolchain = types.SimpleNamespace(mingw=lambda: "")
            self.assertIsNone(build.dav1d_windows(), "the Visual Studio route links no MinGW library")
            build.toolchain = types.SimpleNamespace(mingw=lambda: "llvm-mingw")
            build.LINUX = True
            self.assertIsNone(build.dav1d_windows(), "Linux decodes AV1 through FFmpeg's dav1d")
            build.LINUX = False
            lib.unlink()
            self.assertIsNone(build.dav1d_windows(), "headers without the library")


if __name__ == "__main__":
    unittest.main()
