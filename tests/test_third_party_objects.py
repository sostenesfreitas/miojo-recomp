"""Game-free: the third-party code's objects a release ships (nx2/third_party_objects.py) are taken only by their object key,
and anything that does not fit is compiled as before. No compiler (its answers are stand-ins), no game.

  - a set's object whose key this build has is copied into the work folder's object cache, checked by its SHA-256, and
    nx2/build.py's compile_one links it without starting a compiler;
  - a key the set does not hold (other sources, flags, compiler or headers), a damaged object, a manifest of another format
    or no set: nothing is copied, and compile_one compiles;
  - the stamp in those keys names the compiler's version and what the system headers preprocess to, not their paths;
  - the third-party parts are built at x86-64-v3 on every CPU that has it, not -march=native, so the set fits other CPUs;
  - the release makes the set (scripts/linux/appimage.sh, the Windows workflow) and ships it; the AppImage's check reads it
    back from a player's start.

python -I -B tests/test_third_party_objects.py
"""
import ast
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "nx2" / "build.py"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tpo = load("third_party_objects_under_test", ROOT / "nx2" / "third_party_objects.py")


def build_functions(*names):
    """These functions of nx2/build.py, compiled from its text (the real ones), in a namespace of their own"""
    tree = ast.parse(BUILD.read_text(encoding="utf-8"), filename=str(BUILD))
    found = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {n.name for n in found} == set(names), names
    return compile(ast.Module(body=found, type_ignores=[]), str(BUILD), "exec")


def make_set(folder, objects, stamp="s1", fmt=tpo.FORMAT):
    """A set as write() makes it: objects is {key: bytes}"""
    os.makedirs(folder, exist_ok=True)
    entries = {}
    for key, data in objects.items():
        with open(os.path.join(folder, key + ".o"), "wb") as f:
            f.write(data)
        entries[key] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "source": "third_party/x/" + key + ".c"}
    with open(os.path.join(folder, tpo.MANIFEST), "w", encoding="utf-8") as f:
        json.dump({"format": fmt, "stamp": stamp, "objects": entries}, f)


class Fetch(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(prefix="hookshot-tpo-")
        self.addCleanup(t.cleanup)
        self.root = t.name
        self.set = os.path.join(self.root, "set")
        self.cache = os.path.join(self.root, "work", "objcache")

    def test_a_key_it_holds_is_copied_and_checked(self):
        make_set(self.set, {"a" * 40: b"object a", "b" * 40: b"object b"})
        copied, held, made = tpo.fetch(self.set, self.cache, ["a" * 40, "c" * 40])
        self.assertEqual((copied, held, made), (1, 1, "s1"))
        with open(os.path.join(self.cache, "a" * 40 + ".o"), "rb") as f:
            self.assertEqual(f.read(), b"object a")
        self.assertFalse(os.path.exists(os.path.join(self.cache, "b" * 40 + ".o")))   # (not asked for)
        self.assertFalse(os.path.exists(os.path.join(self.cache, "c" * 40 + ".o")))   # (not held: compiled)
        self.assertFalse([f for f in os.listdir(self.cache) if f.endswith(".tmp")])
        # (already in the cache: held, nothing copied again)
        self.assertEqual(tpo.fetch(self.set, self.cache, ["a" * 40])[:2], (0, 1))

    def test_a_damaged_object_is_not_taken(self):
        make_set(self.set, {"a" * 40: b"object a", "b" * 40: b"object b"})
        with open(os.path.join(self.set, "a" * 40 + ".o"), "wb") as f:
            f.write(b"object A")   # (the same size, other bytes)
        with open(os.path.join(self.set, "b" * 40 + ".o"), "wb") as f:
            f.write(b"obj")   # (cut short)
        self.assertEqual(tpo.fetch(self.set, self.cache, ["a" * 40, "b" * 40])[:2], (0, 0))
        self.assertEqual([f for f in os.listdir(self.cache)], [])
        os.remove(os.path.join(self.set, "b" * 40 + ".o"))   # (gone)
        self.assertEqual(tpo.fetch(self.set, self.cache, ["b" * 40])[:2], (0, 0))

    def test_no_set_or_another_format_is_nothing(self):
        self.assertEqual(tpo.fetch(self.set, self.cache, ["a" * 40]), (0, 0, None))
        make_set(self.set, {"a" * 40: b"object a"}, fmt=tpo.FORMAT + 1)
        self.assertEqual(tpo.fetch(self.set, self.cache, ["a" * 40]), (0, 0, None))
        self.assertEqual(tpo.matching(self.set, ["a" * 40]), set())
        with open(os.path.join(self.set, tpo.MANIFEST), "w") as f:
            f.write("{not json")
        self.assertEqual(tpo.fetch(self.set, self.cache, ["a" * 40]), (0, 0, None))
        self.assertFalse(os.path.exists(self.cache))

    def test_matching_reads_only_the_manifest(self):
        make_set(self.set, {"a" * 40: b"object a"})
        os.remove(os.path.join(self.set, "a" * 40 + ".o"))
        self.assertEqual(tpo.matching(self.set, ["a" * 40, "b" * 40]), {"a" * 40})

    def test_write_then_fetch(self):
        obj = os.path.join(self.root, "x.o")
        with open(obj, "wb") as f:
            f.write(b"\x7fELF built")
        src = str(ROOT / "third_party" / "zstd" / "lib" / "common" / "debug.c")
        made = tpo.write(self.set, [("k" * 40, obj, src)], "stamp")
        self.assertEqual(made["k" * 40]["source"], "third_party/zstd/lib/common/debug.c")
        self.assertEqual(tpo.read_manifest(self.set)["stamp"], "stamp")
        self.assertEqual(tpo.fetch(self.set, self.cache, ["k" * 40])[:2], (1, 1))

    def test_the_switch(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(tpo.folder("/tools"), os.path.join("/tools", tpo.FOLDER))
        for off in ("0", "off", " no "):
            with mock.patch.dict(os.environ, {tpo.ENV: off}):
                self.assertIsNone(tpo.folder("/tools"))
        with mock.patch.dict(os.environ, {tpo.ENV: self.set}):
            self.assertEqual(tpo.folder("/tools"), os.path.abspath(self.set))

    def test_which_sources(self):
        r = str(ROOT)
        for rel in ("third_party/opus/celt/celt.c", "third_party/zstd/lib/common/xxhash.c", "third_party/imgui/imgui.cpp",
                    "third_party/zbic/zstd.c", "gpu/vk/eden/x.cpp", "gpu/vk/sirit/src/sirit.cpp",
                    "gpu/third_party/suyu/shader_recompiler/x.cpp"):
            self.assertTrue(tpo.is_third_party(os.path.join(r, rel), r), rel)
        for rel in ("gpu/vk/vk_draw.cpp", "gpu/vk/vk_vma.cpp", "host/hos_main.c", "gpu/render/rx_core.cpp",
                    "third_party/a64-kit/rt/a64.c", "work/sa/gen/code_001.c"):
            self.assertFalse(tpo.is_third_party(os.path.join(r, rel), r), rel)
        self.assertFalse(tpo.is_third_party("/elsewhere/third_party/opus/x.c", r))


class CompileOne(unittest.TestCase):
    """nx2/build.py's compile_one (the real one): an object the set gave the cache is linked; one it could not give is compiled"""

    def setUp(self):
        t = tempfile.TemporaryDirectory(prefix="hookshot-tpo-compile-")
        self.addCleanup(t.cleanup)
        self.root = t.name
        self.ns = {"os": os, "hashlib": hashlib, "subprocess": subprocess, "time": time, "PLANNED": {}, "GATE": None,
                   "OBJ_CACHE": os.path.join(self.root, "objcache"), "kit": mock.MagicMock(ROOT=self.root),
                   "no_throttle": lambda handle=None: None}
        exec(build_functions("compile_one", "cache_put", "object_key", "flag_text"), self.ns)
        self.src = os.path.join(self.root, "third_party", "zstd", "x.c")
        os.makedirs(os.path.dirname(self.src))
        with open(self.src, "w") as f:
            f.write("int x;\n")
        self.obj = os.path.join(self.root, "obj", "zstd_x.o")
        os.makedirs(os.path.dirname(self.obj))
        self.flags = ["-march=x86-64-v3", "-O2"]

    def run_job(self, deps):
        self.ns["PLANNED"].clear()   # (build.py's plan names each job's key from its own flags and headers before the compile)
        job = ("clang", self.src, self.obj, self.flags, deps)
        key = self.ns["object_key"](self.src, self.flags, deps)
        compiler = mock.MagicMock(side_effect=OSError("the compiler was started"))
        with mock.patch.object(subprocess, "Popen", compiler):
            try:
                result = self.ns["compile_one"](job)
            except OSError:
                result = None
        return key, result, compiler.called

    def test_the_set_s_object_is_linked_and_another_key_compiles(self):
        made_with = b"third-party stamp-of-the-release"
        key = self.ns["object_key"](self.src, self.flags, made_with)
        make_set(os.path.join(self.root, "set"), {key: b"the release's object"})
        # (this build's keys: the same stamp -> the set's object, no compiler)
        copied, held, _ = tpo.fetch(os.path.join(self.root, "set"), self.ns["OBJ_CACHE"], [key])
        self.assertEqual((copied, held), (1, 1))
        _, result, compiled = self.run_job(made_with)
        self.assertFalse(compiled)
        self.assertEqual(result, (self.src, 0.0, None))
        with open(self.obj, "rb") as f, open(self.obj + ".sha1") as stamp:
            self.assertEqual((f.read(), stamp.read()), (b"the release's object", key))
        # (another compiler or other headers on this PC: another key, which the set does not hold, so it is compiled)
        os.remove(self.obj)
        other, _, _ = self.run_job(b"third-party stamp-of-this-pc")
        self.assertNotEqual(other, key)
        self.assertEqual(tpo.fetch(os.path.join(self.root, "set"), self.ns["OBJ_CACHE"], [other])[:2], (0, 0))
        _, _, compiled = self.run_job(b"third-party stamp-of-this-pc")
        self.assertTrue(compiled)
        # (other flags, the same stamp: compiled too)
        self.flags = ["-march=native", "-O2"]
        _, _, compiled = self.run_job(made_with)
        self.assertTrue(compiled)


class Stamp(unittest.TestCase):
    def answers(self, version="clang version 23.1.5 (https://example/llvm abc)\nTarget: x86_64-unknown-linux-gnu\n"
                              "InstalledDir: /tmp/.mount_a/usr/bin\n", c=b"int c;\n#define __GLIBC_MINOR__ 35\n",
                cpp=b"namespace std {}\n", fail=False, driver='"-pic-level" "2" "-target-cpu" "x86-64-v3"',
                where="/tmp/.mount_a"):
        def run(cmd, **kw):
            if "--version" in cmd:
                return subprocess.CompletedProcess(cmd, 1 if fail else 0, version, "")
            if "-###" in cmd:   # (the driver's cc1 line, with the paths a PC's install gives it)
                cc1 = '"%s/usr/bin/clang-23" "-cc1" "-triple" "x86_64-unknown-linux-gnu" %s "-resource-dir" "%s/usr/lib/clang/23" ' \
                      '"-fdebug-compilation-dir=%s/work" "-o" "%s"' % (where, driver, where, where, os.devnull)
                return subprocess.CompletedProcess(cmd, 0, "", "clang version 23\nInstalledDir: %s/usr/bin\n %s\n" % (where, cc1))
            lang = cmd[cmd.index("-x") + 1]
            self.assertIn("-P", cmd)   # (no line markers: no paths)
            self.assertIn("-dD", cmd)   # (the macros: the libraries' versions)
            return subprocess.CompletedProcess(cmd, 0, c if lang == "c" else cpp, b"")
        return run

    def stamp(self, **kw):
        with mock.patch.object(tpo.subprocess, "run", self.answers(**kw)):
            return tpo.stamp("clang", "clang++", ["--sysroot=/x"], ["-march=x86-64-v3"])

    def test_what_it_names(self):
        first = self.stamp()
        self.assertRegex(first, r"^[0-9a-f]{64}$")
        # (the compiler's folder is not in it: the AppImage's mount and the tools' folder are new paths on every PC)
        self.assertEqual(first, self.stamp(version="clang version 23.1.5 (https://example/llvm abc)\nTarget: x86_64-unknown-"
                                                   "linux-gnu\nInstalledDir: /home/p/tools/usr/bin\n"))
        self.assertNotEqual(first, self.stamp(version="clang version 23.1.6 (https://example/llvm def)\n"))
        self.assertNotEqual(first, self.stamp(c=b"int c;\n#define __GLIBC_MINOR__ 39\n"))   # (another glibc)
        self.assertNotEqual(first, self.stamp(cpp=b"namespace std { inline namespace __1 {} }\n"))   # (another C++ library)
        # (the same version built with other defaults: PIE off, another CPU; and the same install somewhere else)
        self.assertNotEqual(first, self.stamp(driver='"-pic-level" "0" "-target-cpu" "x86-64-v3"'))
        self.assertEqual(first, self.stamp(where="C:\\Users\\p\\HookshotBuilder\\tools"))
        self.assertEqual(self.stamp(fail=True), "")   # (no answer: no set fits)

    def test_the_vulkan_headers_are_in_it(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "vk", "include", "vulkan")
            os.makedirs(p)
            with open(os.path.join(p, "vulkan_core.h"), "w") as f:
                f.write("#define VK_HEADER_VERSION 345\n")
            with mock.patch.object(tpo.subprocess, "run", self.answers()):
                one = tpo.stamp("clang", "clang++", [], [], [os.path.join(d, "vk"), None])
                with open(os.path.join(p, "vulkan_core.h"), "w") as f:
                    f.write("#define VK_HEADER_VERSION 346\n")
                two = tpo.stamp("clang", "clang++", [], [], [os.path.join(d, "vk"), None])
            self.assertNotEqual(one, two)


class BuildScript(unittest.TestCase):
    def setUp(self):
        self.src = BUILD.read_text(encoding="utf-8")

    def test_third_party_cpu_level(self):
        start = self.src.index("THIRD_PARTY_CPU = ")
        block = self.src[start:self.src.index("OPT = ", start)]
        for env, march, cpu, want in (({}, "native", ["-march=native", "-ffp-contract=off", "-DHOOKSHOT_NATIVE_CPU=z"],
                                       ["-march=x86-64-v3", "-ffp-contract=off"]),
                                      ({}, "x86-64-v3", ["-march=x86-64-v3", "-ffp-contract=off"],
                                       ["-march=x86-64-v3", "-ffp-contract=off"]),
                                      ({}, "x86-64-v2", ["-march=x86-64-v2", "-ffp-contract=off"],
                                       ["-march=x86-64-v2", "-ffp-contract=off"]),   # (a CPU below v3: as the rest, compiled)
                                      ({"RX_MARCH": "native"}, "native", ["-march=native", "-ffp-contract=off"],
                                       ["-march=native", "-ffp-contract=off"])):
            ns = {"os": os, "MARCH": march, "CPU": cpu}
            with mock.patch.dict(os.environ, env, clear=True):
                exec(block, ns)
            self.assertEqual(ns["THIRD_PARTY_CPU"], want, (env, march))

    def test_main_takes_the_set_before_the_plan_and_only_for_marked_jobs(self):
        main = self.src[self.src.index("\ndef main("):]
        self.assertIn("tp_jobs = [j for j in jobs if j[4].endswith(tp_deps) and third_party_objects.is_third_party(j[1])]", main)
        fetch = main.index("third_party_objects.fetch(tp_folder, OBJ_CACHE")
        self.assertLess(main.index("if vk_only:"), fetch)   # (the jobs as they will be compiled)
        self.assertLess(fetch, main.index("work_jobs, keys = plan_jobs(jobs)"))   # (the countdown does not count them)
        self.assertLess(fetch, main.index("ex.submit(compile_one, job)"))
        self.assertIn("tp_stamp = third_party_objects.stamp(cc, toolchain.find(\"clang++\"), target(), THIRD_PARTY_CPU, "
                      "vulkan_headers())", main)
        # (the parts built for the set: the stamp in their headers, x86-64-v3)
        self.assertIn('oflags, b"opus-1.5.2" + tp_deps', main)
        self.assertIn("vk_lib_deps + tp_deps if lib else", main)
        self.assertIn("(tp_vflags if lib else vflags + vk_pgo_flags)", main)
        self.assertIn("imgui_deps + tp_deps", main)
        self.assertIn("zstd_deps + tp_deps", main)
        self.assertIn("gdeps + tp_deps if third_party_objects.is_third_party(src) else gdeps", main)
        # (the release's run: those jobs alone, written as the set, nothing linked)
        made = main.index("third_party_objects.write(a.third_party_objects")
        self.assertLess(made, main.index("if a.standalone and not vk_only and LINUX:"))
        self.assertIn("jobs = tp_jobs", main)
        # (a player's dry run says how many the set holds: the AppImage's check)
        self.assertIn('"dry run: the third-party objects: %d of %d in %s"', main)


class Release(unittest.TestCase):
    # (Hookshot's release packaging, its own AppImage and Windows parts, makes no prebuilt set yet: a build compiles them as
    # before. What is checked here is make itself)
    def test_make_builds_for_v3_with_no_store(self):
        calls = []

        def run(cmd, env=None, **kw):
            calls.append((cmd, env))
            return subprocess.CompletedProcess(cmd, 0)
        with tempfile.TemporaryDirectory() as d, mock.patch.object(tpo.subprocess, "run", run), \
                mock.patch.dict(os.environ, {"HOOKSHOT_OBJ_STORE": "/store", tpo.ENV: "0"}):
            self.assertEqual(tpo.make(os.path.join(d, "out"), 3), 0)
        cmd, env = calls[0]
        self.assertIn("--third-party-objects", cmd)
        self.assertIn("--standalone", cmd)
        self.assertEqual(env["RX_MARCH"], "x86-64-v3")
        self.assertNotIn("HOOKSHOT_OBJ_STORE", env)
        self.assertNotIn(tpo.ENV, env)
        self.assertFalse(os.path.exists(env["OOT_WORK"]))   # (its own work folder, removed after)


if __name__ == "__main__":
    unittest.main()
