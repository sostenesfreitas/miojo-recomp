"""Run the actual Vulkan job construction and object keys over authored files, without a compiler or game.

An alternate --build-script permits the same regression to exercise an older production build.py.
"""
import argparse
import ast
import hashlib
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import warnings


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "nx2" / "build.py"


def production_code(path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    names = {"tree_headers", "vk_sources", "flag_text", "object_key", "zstd_jobs"}
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {n.name for n in functions} == names
    main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "main")
    # Execute the real branch that hashes the translator, chooses per-source flags/deps and appends Vulkan jobs.
    branches = [n for n in ast.walk(main) if isinstance(n, ast.If) and any(
        isinstance(s, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "vflags" for t in s.targets)
        for s in n.body)]
    assert len(branches) == 1, "expected one Vulkan job-construction branch"
    initial = next(n for n in main.body if isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id == "vk_jobs" for t in n.targets))
    return compile(ast.Module(body=functions + [initial] + branches[0].body, type_ignores=[]), str(path), "exec")


class ShaderStampCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-shader-stamp-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.files = {
            "gpu/vk/vk_shader.cpp": "int shader;\n",
            "gpu/vk/vk_bank.cpp": "int bank;\n",
            "gpu/vk/vk_draw.cpp": "int draw;\n",
            "gpu/vk/vk_menu.cpp": "int menu;\n",
            "gpu/vk/vk_state.h": "struct State {};\n",
            "gpu/vk/vk_api.h": "struct Api {};\n",
            "gpu/vk/vk_dlss.cpp": "int dlss;\n",
            "gpu/vk/vk_fsr4.cpp": "int fsr;\n",
            "gpu/vk/vk_ngx_msvc_rt.cpp": "int msvc_rt;\n",
            "host/mingw_msvc_shims.c": "int shims;\n",
            "gpu/vk/eden/translate.cpp": "int translator;\n",
            "gpu/vk/eden/ir.h": "struct IR {};\n",
            "gpu/vk/sirit/src/sirit.cpp": "int sirit;\n",
            "gpu/vk/sirit/include/sirit.h": "struct Sirit {};\n",
            "gpu/vk/compat/compat.h": "struct Compat {};\n",
            "gpu/compat/common.h": "struct Common {};\n",
            "gpu/render/render.h": "struct Render {};\n",
            "gpu/third_party/suyu/video_core/textures/decoders.cpp": "int decoder;\n",
            "gpu/third_party/suyu/video_core/textures/astc.cpp": "int astc;\n",
            "gpu/third_party/suyu/video_core/textures/texture.h": "struct Texture {};\n",
            "host/posix/windows.h": "struct Posix {};\n",
            "host/posix/win32_posix.c": "int win32;\n",
            "gen/ui_fonts.cpp": "int fonts;\n",
            "third_party/imgui/imgui.h": "struct ImGui {};\n",
            "third_party/imgui/backends/imgui_impl_vulkan.h": "struct ImGuiVulkan {};\n",
            "third_party/imgui/imgui.cpp": "int imgui;\n",
            "third_party/imgui/imgui_draw.cpp": "int imgui_draw;\n",
            "third_party/imgui/imgui_tables.cpp": "int imgui_tables;\n",
            "third_party/imgui/imgui_widgets.cpp": "int imgui_widgets;\n",
            "third_party/imgui/backends/imgui_impl_vulkan.cpp": "int imgui_vulkan;\n",
            "third_party/zstd/lib/zstd.h": "int z0;\n",
            "third_party/zstd/lib/common/debug.c": "int z1;\n",
            "third_party/zstd/lib/common/entropy_common.c": "int z2;\n",
            "third_party/zstd/lib/common/error_private.c": "int z3;\n",
            "third_party/zstd/lib/common/fse_decompress.c": "int z4;\n",
            "third_party/zstd/lib/common/xxhash.c": "int z5;\n",
            "third_party/zstd/lib/common/zstd_common.c": "int z6;\n",
            "third_party/zstd/lib/decompress/huf_decompress.c": "int z7;\n",
            "third_party/zstd/lib/decompress/zstd_ddict.c": "int z8;\n",
            "third_party/zstd/lib/decompress/zstd_decompress.c": "int z9;\n",
            "third_party/zstd/lib/decompress/zstd_decompress_block.c": "int z10;\n",
            "third_party/zbic/zstd.c": "int zstd_zbic;\n",
            "third_party/zbic/zstd.h": "int zstd_zbic_h;\n",
            "third_party/zbic/zstd_errors.h": "int zstd_zbic_errors;\n",
        }
        for name, text in self.files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode())
        self.code = production_code(BUILD_SCRIPT)

    def jobs(self, linux, version="fixture", release=False, ngx="", mingw="llvm-mingw", fsr=False):
        namespace = {
            "os": os, "hashlib": hashlib,
            "GPU": str(self.root / "gpu"), "POSIX": str(self.root / "host/posix"), "HOST": str(self.root / "host"),
            "LINUX": linux, "CPU": ["-march=x86-64"],
            "kit": SimpleNamespace(ROOT=str(self.root), HERE=str(self.root / "nx2")),
            "toolchain": SimpleNamespace(find=lambda name: name, need=lambda name: name, mingw=lambda: mingw),
            "target": lambda: ["--target=x86_64-linux-gnu" if linux else "--target=x86_64-w64-mingw32"],
            "cpm_package": lambda name: str(self.root / "deps" / name),
            "spirv_headers": str(self.root / "deps/spirv-headers"),
            "vk_headers": str(self.root / "deps/vulkanheaders"),
            "vma_headers": str(self.root / "deps/vulkanmemoryallocator"),
            "obj_dir": str(self.root / "obj"), "cc": "clang", "jobs": [], "ZSTD": str(self.root / "third_party/zstd/lib"),
            "stamp_def": '-DHOOKSHOT_VERSION="%s"' % version, "logo_text": b"", "gen_dir": str(self.root / "gen"),
            "fonts_cpp": str(self.root / "gen/ui_fonts.cpp"),
            # (the back end's branch makes its logo header and fonts file itself since #1567: no files written here)
            "logo_rgba": SimpleNamespace(write=lambda path: ""),
            "ui_fonts": SimpleNamespace(write=lambda path: None),
            "NGX_SDK": ngx,   # ("": no NVIDIA NGX SDK, vk_dlss.cpp built without DLSS, as by default)
            "NGX_MSVC_RT": "vk_ngx_msvc_rt.cpp",
            "FSR": fsr,   # (HOOKSHOT_FSR=1: vk_fsr4.cpp built with AMD FSR 4)
        }
        with mock.patch.dict(os.environ, {"RX_RELEASE": "1"} if release else {}, clear=True), warnings.catch_warnings():
            # The production key/header helpers use short-lived open(...).read() expressions.
            warnings.simplefilter("ignore", ResourceWarning)
            exec(self.code, namespace)
            return {str(Path(src).relative_to(self.root)).replace("\\", "/"):
                    {"key": namespace["object_key"](src, flags, deps), "flags": flags}
                    for _, src, _, flags, deps in namespace["vk_jobs"]}

    def changed(self, before, after):
        self.assertEqual(before.keys(), after.keys())
        return {name for name in before if before[name]["key"] != after[name]["key"]}

    def test_translator_source_changes_only_its_object_and_shader_consumer(self):
        path = self.root / "gpu/vk/eden/translate.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int translator;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int translator_changed;\n")
                after = self.jobs(linux)
                self.assertEqual(self.changed(before, after),
                                 {"gpu/vk/eden/translate.cpp", "gpu/vk/vk_shader.cpp"})

    def test_own_renderer_source_still_invalidates_its_object(self):
        path = self.root / "gpu/vk/vk_draw.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int draw;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int draw_changed;\n")
                self.assertEqual(self.changed(before, self.jobs(linux)), {"gpu/vk/vk_draw.cpp"})

    def test_bank_source_changes_only_its_object_and_shader_consumer(self):
        path = self.root / "gpu/vk/vk_bank.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int bank;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int bank_changed;\n")
                self.assertEqual(self.changed(before, self.jobs(linux)),
                                 {"gpu/vk/vk_bank.cpp", "gpu/vk/vk_shader.cpp"})

    def test_headers_still_invalidate_the_existing_dependency_groups(self):
        path = self.root / "gpu/vk/eden/ir.h"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"struct IR {};\n")
                before = self.jobs(linux)
                path.write_bytes(b"struct IR { int changed; };\n")
                # (Dear ImGui's objects and zstd's see only their own headers; the fonts' file none)
                expected = {name for name in before if not name.startswith(("host/posix/", "third_party/imgui/", "third_party/zstd/", "third_party/zbic/", "gen/"))}
                self.assertEqual(self.changed(before, self.jobs(linux)), expected)

    def test_stamp_line_ending_normalization_is_unchanged(self):
        path = self.root / "gpu/vk/eden/translate.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int translator;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int translator;\r\n")
                self.assertEqual(self.changed(before, self.jobs(linux)), {"gpu/vk/eden/translate.cpp"})

    def test_stamp_version_and_release_flags_reach_their_consumers(self):
        for linux in (False, True):
            with self.subTest(linux=linux):
                before = self.jobs(linux, release=True)
                for name, job in before.items():
                    shader = [f for f in job["flags"] if f.startswith("-DHOOKSHOT_SHADER_STAMP=")]
                    version = [f for f in job["flags"] if f.startswith("-DHOOKSHOT_VERSION=")]
                    self.assertEqual(len(shader), int(name == "gpu/vk/vk_shader.cpp"))
                    self.assertEqual(len(version), int(name == "gpu/vk/vk_menu.cpp"))
                    if not name.startswith(("host/posix/", "third_party/zstd/", "third_party/zbic/", "gen/")):   # (plain C libraries, the fonts' bytes)
                        self.assertIn("-DHOOKSHOT_RELEASE", job["flags"])
                self.assertEqual(self.changed(before, self.jobs(linux, version="new", release=True)),
                                 {"gpu/vk/vk_menu.cpp"})

    def test_ngx_sdk_reaches_only_vk_dlss_and_the_mingw_runtime_shims(self):
        sdk = str(self.root / "ngx-sdk")
        for linux, mingw in ((False, "llvm-mingw"), (False, ""), (True, "")):
            with self.subTest(linux=linux, mingw=bool(mingw)):
                plain = self.jobs(linux, mingw=mingw)
                self.assertNotIn("gpu/vk/vk_ngx_msvc_rt.cpp", plain)   # (never without the SDK)
                self.assertNotIn("host/mingw_msvc_shims.c", plain)
                self.assertFalse([n for n, j in plain.items() if "-DHOOKSHOT_NGX" in j["flags"]])
                with mock.patch("builtins.print"):
                    ngx = self.jobs(linux, ngx=sdk, mingw=mingw)
                shims = {"gpu/vk/vk_ngx_msvc_rt.cpp", "host/mingw_msvc_shims.c"}
                # (the shims only for llvm-mingw: the MSVC build (HOOKSHOT_MSVC=1) and Linux have their own runtime)
                self.assertEqual(set(ngx) - set(plain), shims if mingw and not linux else set())
                self.assertEqual({n for n, j in ngx.items() if "-DHOOKSHOT_NGX" in j["flags"]},
                                 {"gpu/vk/vk_dlss.cpp"} | ({"gpu/vk/vk_ngx_msvc_rt.cpp"} if mingw and not linux else set()))
                self.assertEqual(self.changed(plain, {n: ngx[n] for n in plain}), {"gpu/vk/vk_dlss.cpp"})

    def test_fsr_reaches_only_vk_fsr4(self):
        for linux in (False, True):
            with self.subTest(linux=linux):
                plain = self.jobs(linux)
                self.assertFalse([n for n, j in plain.items() if "-DHOOKSHOT_FSR" in j["flags"]])
                with mock.patch("builtins.print"):
                    fsr = self.jobs(linux, fsr=True)
                self.assertEqual(set(fsr), set(plain))   # (no other files: AMD's headers alone, nothing linked)
                self.assertEqual({n for n, j in fsr.items() if "-DHOOKSHOT_FSR" in j["flags"]}, {"gpu/vk/vk_fsr4.cpp"})
                self.assertEqual(self.changed(plain, fsr), {"gpu/vk/vk_fsr4.cpp"})


class NgxSdkTests(unittest.TestCase):
    """nx2/build.py's HOOKSHOT_NGX_SDK: which library each platform needs, and what the Windows link adds"""
    def setUp(self):
        tree = ast.parse(BUILD_SCRIPT.read_text(encoding="utf-8"), filename=str(BUILD_SCRIPT))
        names = {"ngx_library", "ngx_sdk", "ngx_link"}
        functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        self.assertEqual({n.name for n in functions}, names)
        self.ns = {"os": os}
        exec(compile(ast.Module(body=functions, type_ignores=[]), str(BUILD_SCRIPT), "exec"), self.ns)
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-ngx-")
        self.addCleanup(self.temp.cleanup)
        self.sdk = Path(self.temp.name)
        (self.sdk / "lib").mkdir()

    def quiet(self, sdk, linux):
        with mock.patch("builtins.print"):
            return self.ns["ngx_sdk"](sdk, linux)

    def test_each_platform_needs_its_own_library(self):
        sdk = str(self.sdk)
        self.assertEqual(self.quiet("", True), "")
        self.assertEqual(self.quiet(sdk, True), "")
        self.assertEqual(self.quiet(sdk, False), "")
        (self.sdk / "lib" / "nvsdk_ngx_s.lib").write_bytes(b"")
        self.assertEqual(self.quiet(sdk, False), sdk)   # (Windows: the static /MT library)
        self.assertEqual(self.quiet(sdk, True), "")
        (self.sdk / "lib" / "libnvsdk_ngx.a").write_bytes(b"")
        self.assertEqual(self.quiet(sdk, True), sdk)

    def test_windows_link(self):
        sdk = str(self.sdk)
        self.assertEqual(self.ns["ngx_link"]("", "llvm-mingw"), [])
        self.assertEqual(self.ns["ngx_link"]("", ""), [])
        lib = os.path.join(sdk, "lib", "nvsdk_ngx_s.lib")
        self.assertEqual(self.ns["ngx_link"](sdk, "llvm-mingw"), [lib, "-ladvapi32", "-Wl,-Xlink=/nodefaultlib:libcpmt"])
        self.assertEqual(self.ns["ngx_link"](sdk, ""), [lib, "-ladvapi32"])   # (MSVC: its own runtime, nothing left out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-script", type=Path, default=BUILD_SCRIPT)
    args, rest = parser.parse_known_args()
    BUILD_SCRIPT = args.build_script
    unittest.main(argv=[sys.argv[0], *rest])
