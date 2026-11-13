"""Linker options that name a file reach the linker whole, whatever the build files folder is called (nx2/build.py linker).

The link map (hookshot.map, the Vulkan back end's map) and the link order (hot_order.txt) are in the build files folder,
which the player chooses (make_game.py --work, the builders' build files folder). The compiler splits a -Wl, option at
every comma, and toolchain.link_opt is one, so a build files folder named x,y stopped the link of the game:

    ld.lld: error: cannot open /home/.../work/x: No such file or directory
    ld.lld: error: cannot open y/sa/build/hookshot.map: No such file or directory
    ld.lld: error: cannot open y/hot_order.txt: No such file or directory

(lld-link and llvm-mingw's lld the same with /map: and /order:@). build.py's linker() passes them through -Xlinker, one
argument each. Checked here:
  - nx2/build.py puts nothing but a constant in a -Wl, option, and gives toolchain.link_opt only constants;
  - linker() and HOT_ORDER() as build.py has them, for Linux, the MSVC target and llvm-mingw, keep a folder named
    "Games, Zelda" whole;
  - with clang and LLVM's lld on this PC: a small program linked into that folder with build.py's options, through each
    linker found (ld.lld, lld-link, ld.lld's MinGW driver), writes its map there. Skipped where they are not installed.

--build-script <build.py>: the same checks against another build.py (main before linker() fails every one).

python -I -B tests/test_link_path_comma.py
"""
import argparse
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "nx2" / "build.py"
NAMES = ("linker", "HOT_ORDER")
# (route, clang's target, build.py's LINUX, toolchain.mingw(), the linker clang runs for it, its entry options and map option.
# The entry is start, not main: clang's MinGW target makes main call __main, from a runtime this link has not)
ROUTES = (("Linux", "x86_64-unknown-linux-gnu", True, "", "ld.lld", ("--entry=start",), "-Map="),
          ("MSVC target", "x86_64-pc-windows-msvc", False, "", "lld-link", ("/entry:start", "/subsystem:console"), "/map:"),
          ("llvm-mingw", "x86_64-w64-mingw32", False, "llvm-mingw", "ld.lld", ("/entry:start", "/subsystem:console"), "/map:"))


def build_tree():
    return ast.parse(BUILD_SCRIPT.read_text(encoding="utf-8"), filename=str(BUILD_SCRIPT))


def link_functions(linux, mingw, work):
    """linker and HOT_ORDER as build.py has them, for that platform, with `work` as the build files folder"""
    functions = [n for n in build_tree().body if isinstance(n, ast.FunctionDef) and n.name in NAMES]
    missing = sorted(set(NAMES) - {n.name for n in functions})
    if missing:
        raise AssertionError("%s has no %s" % (BUILD_SCRIPT, ", ".join(n + "()" for n in missing)))
    ns = {"os": os, "LINUX": linux, "kit": types.SimpleNamespace(WORK=work),
          "toolchain": types.SimpleNamespace(mingw=lambda: mingw)}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(BUILD_SCRIPT), "exec"), ns)
    return ns["linker"], ns["HOT_ORDER"]


def wl_constant(node):
    """The "-Wl,..." text an expression starts with, or None"""
    while isinstance(node, ast.BinOp):
        node = node.left
    if isinstance(node, ast.JoinedStr) and node.values:
        node = node.values[0]
    if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.startswith("-Wl,"):
        return node.value
    return None


class NoValueThroughWl(unittest.TestCase):
    def test_every_wl_option_is_a_constant(self):
        found, inside = [], set()
        for node in ast.walk(build_tree()):
            if id(node) in inside:
                continue
            made = isinstance(node, (ast.BinOp, ast.JoinedStr)) and wl_constant(node) is not None
            made = made or (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and
                            node.func.attr == "link_opt" and not all(isinstance(a, ast.Constant) for a in node.args))
            if made:
                found.append((node.lineno, "line %d: %s" % (node.lineno, ast.unparse(node))))
                inside.update(id(n) for n in ast.walk(node))
        found = [text for _, text in sorted(found)]
        self.assertEqual(found, [], "a -Wl, option (or toolchain.link_opt, which is one) with a value made at build time: "
                         "the compiler splits it at every comma in that value. Pass it with linker():\n  " +
                         "\n  ".join(found))


class LinkerCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="hookshot-link-comma-")
        self.addCleanup(tmp.cleanup)
        self.work = os.path.join(tmp.name, "Games, Zelda")
        os.makedirs(self.work)
        self.order = os.path.join(self.work, "hot_order.txt")
        Path(self.order).write_text("start\n", encoding="utf-8")
        env = mock.patch.dict(os.environ)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("RX_NO_ORDER", None)


class LinkerKeepsPathsWhole(LinkerCase):
    def test_linux(self):
        linker, hot_order = link_functions(True, "", self.work)
        mapfile = os.path.join(self.work, "sa", "build", "hookshot.map")
        self.assertEqual(linker("-Map=" + mapfile), ["-Xlinker", "-Map=" + mapfile])
        self.assertEqual(hot_order(), ["-Xlinker", "--symbol-ordering-file=" + self.order,
                                       "-Xlinker", "--no-warn-symbol-ordering"])

    def test_msvc_target(self):
        linker, hot_order = link_functions(False, "", self.work)
        self.assertEqual(linker("/map:C:/Games, Zelda/hookshot.map"), ["-Xlinker", "/map:C:/Games, Zelda/hookshot.map"])
        self.assertEqual(hot_order(), ["-Xlinker", "/order:@" + self.order.replace("\\", "/"), "-Xlinker", "/ignore:4037"])

    def test_llvm_mingw(self):
        linker, hot_order = link_functions(False, r"C:\llvm-mingw", self.work)
        self.assertEqual(linker("/map:C:/Games, Zelda/hookshot.map"),
                         ["-Xlinker", "-Xlink=/map:C:/Games, Zelda/hookshot.map"])
        self.assertEqual(hot_order(), ["-Xlinker", "-Xlink=/order:@" + self.order.replace("\\", "/"),
                                       "-Xlinker", "-Xlink=/ignore:4037"])

    def test_no_order_without_the_list(self):
        os.remove(self.order)
        self.assertEqual(link_functions(True, "", self.work)[1](), [])


class LinkIntoAFolderWithAComma(LinkerCase):
    def test_each_linker_writes_the_map_there(self):
        clang = shutil.which("clang")
        if not clang:
            self.skipTest("no clang")
        src = os.path.join(self.work, "t.c")
        Path(src).write_text("int start(void) { return 0; }\n", encoding="utf-8")
        for name, target, linux, mingw, lld, entry, map_opt in ROUTES:
            with self.subTest(name):
                if not (shutil.which(lld, path=os.path.dirname(clang)) or shutil.which(lld)):
                    self.skipTest("no %s" % lld)
                linker, hot_order = link_functions(linux, mingw, self.work)
                obj = os.path.join(self.work, target + ".o")
                exe = os.path.join(self.work, target + ("" if linux else ".exe"))
                r = subprocess.run([clang, "--target=" + target, "-c", src, "-o", obj], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                r = subprocess.run([clang, "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-o", exe, obj] +
                                   linker(*entry, map_opt + exe + ".map") + hot_order(), capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertTrue(os.path.isfile(exe + ".map"), "no map in " + self.work)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-script", type=Path, default=BUILD_SCRIPT)
    args, rest = parser.parse_known_args()
    BUILD_SCRIPT = args.build_script
    unittest.main(argv=[sys.argv[0], *rest])
