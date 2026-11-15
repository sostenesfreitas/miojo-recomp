"""Game-free: a player's build starts from the tools the Linux AppImage carries. Runs the real tools-copy block of
scripts/linux/appimage.sh into a temporary folder, then nx2/build.py --standalone --dry-run from there, as the AppImage runs
it (the Vulkan back end compiled from the image's sources: the AppImage carries no built one since 2026-11-14), with
stand-ins for the compiler, sysroot, FFmpeg and Vulkan headers it brings.
The dry run goes up to the compile, where the lifted game is first needed, and fails on any other file the compile would
read that is not in the tree. 2026-11-10: nx2/build.py wrote the menu's fonts from third_party/fonts at every start, the
AppImage does not carry that folder, and every Linux player's build stopped (#1567); CI had packaged the AppImage and never
started a build from it. The AppImage workflow does the same with the real image: scripts/linux/check_player_build.sh.

    python -I -B tests/test_appimage_player_build.py
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def package(tools):
    """appimage.sh's own copy of the tools into <tools> (its T). Without -e: the native helpers it copies (zbic.so) are
    built by appimage.sh first, and what the build needs of the tree is the dry run's to say"""
    script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
    start = script.index("# ---- the tools (the sources")
    end = script.index("# ---- what it builds with", start)
    subprocess.run(["bash", "-u", "-c", script[start:end]], cwd=ROOT, env=dict(os.environ, T=str(tools), VERSION="test"),
                   capture_output=True, text=True)


def dry_run(tools, lib, work, **extra):
    """nx2/build.py as nx2/make_game.py runs it in the AppImage (AppRun's variables), with nothing else of this PC's"""
    env = {**extra, "PATH": "/usr/bin:/bin", "HOME": str(work / "home"), "LANG": "C.UTF-8", "HOOKSHOT_TOOLS": str(tools),
           "EMSDK": str(lib / "emsdk"), "HOOKSHOT_SYSROOT": str(lib / "sysroot"), "HOOKSHOT_FFMPEG": str(lib / "ffmpeg"),
           "OOT_WORK": str(work / "work"), "RX_RELEASE": "1", "A64_HOT_CHUNK": "1", "RX_MARCH": "x86-64-v3"}
    return subprocess.run([sys.executable, "-I", "-B", "-u", "-X", "utf8", "nx2/build.py", "--standalone", "--jobs", "1",
                           "--dry-run"], cwd=tools, env=env, capture_output=True, text=True, timeout=300)


@unittest.skipUnless(sys.platform.startswith("linux"), "the AppImage is made on Linux")
class PlayerBuildFromTheAppImage(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hookshot-appimage-build-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.tools, self.lib = self.root / "AppDir/usr/share/hookshot", self.root / "AppDir/usr/lib/hookshot"
        self.tools.mkdir(parents=True)
        package(self.tools)
        # (stand-ins for what appimage.sh brings: a compiler that is never run, the sysroot, FFmpeg, and the Vulkan headers in
        # their CPM layout in the image's cpm/, which nx2/build.py finds beside the tools)
        (self.lib / "emsdk/upstream/bin").mkdir(parents=True)
        for name in ("clang", "clang++"):
            clang = self.lib / "emsdk/upstream/bin" / name
            clang.write_text("#!/bin/sh\nexit 1\n")
            clang.chmod(0o755)
        for d in ("sysroot", "ffmpeg/include", "ffmpeg/lib"):
            (self.lib / d).mkdir(parents=True)
        for probe in ("vulkanheaders/v1/include/vulkan/vulkan.h", "spirv-headers/v1/include/spirv/unified1/spirv.h",
                      "vulkanmemoryallocator/v1/include/vk_mem_alloc.h"):
            (self.tools / "cpm" / probe).parent.mkdir(parents=True, exist_ok=True)
            (self.tools / "cpm" / probe).write_text("")
        (self.root / "home").mkdir()

    def test_starts_from_the_packaged_tree(self):
        r = dry_run(self.tools, self.lib, self.root)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("the Vulkan back end: Vulkan", r.stdout)   # (compiled from the image's sources: no prebuilt one)
        self.assertRegex(r.stdout, r"dry run: [1-9]\d* files to compile besides the lift's, 0 missing")
        self.assertFalse(list(self.tools.rglob("__pycache__")))   # (the image is read-only on a player's PC)

    def test_fsr_compiles_the_back_end_from_the_packaged_tree(self):
        # (AMD FSR 4 chosen: the build compiles gpu/vk from the image's tree with AMD's FSR API headers, which the image
        # carries in third_party/fidelityfx)
        r = dry_run(self.tools, self.lib, self.root, HOOKSHOT_FSR="1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("with AMD FSR 4", r.stdout)
        self.assertRegex(r.stdout, r"dry run: [1-9]\d* files to compile besides the lift's, 0 missing")

    def test_the_image_carries_the_renderer_sources_and_assets(self):
        # (every player's build compiles the back end: its sources and the committed play profile, assets/profiles/)
        self.assertTrue((self.tools / "gpu/vk/vk_main.cpp").is_file())
        self.assertTrue((self.tools / "assets/profiles").is_dir())

    def test_a_file_the_image_lacks_fails(self):
        first = (self.tools / "third_party/opus/sources.txt").read_text().split()[0]
        (self.tools / "third_party/opus" / first).unlink()
        r = dry_run(self.tools, self.lib, self.root)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("missing: " + str(self.tools / "nx2" / ".." / "third_party" / "opus" / first), r.stdout)


class TheChecksAreWired(unittest.TestCase):
    def test_the_workflow_checks_the_image_it_made(self):
        flow = (ROOT / ".github/workflows/linux-appimage.yml").read_text(encoding="utf-8")
        self.assertLess(flow.index("run: scripts/linux/appimage.sh"), flow.index("run: scripts/linux/check_player_build.sh"))

    def test_apprun_sets_up_before_python(self):
        # (check_player_build.sh takes AppRun's environment from its lines before PY=)
        apprun = (ROOT / "builder/linux/AppRun").read_text(encoding="utf-8")
        head = apprun[:apprun.index("\nPY=")]
        self.assertNotRegex(head, r"\n\s*(exec|exit)\b")
        script = (ROOT / "scripts/linux/check_player_build.sh").read_text(encoding="utf-8")
        self.assertIn("sed '/^PY=/,$d'", script)
        self.assertIn("--dry-run", script)

    def test_the_image_folders_are_set_before_use(self):
        # (2026-11-14: a merge took out `L=` with the prebuilt renderer while the build tools still go into $L, and the AppImage
        # build stopped at "L: unbound variable" under set -u; the tools-copy run above stops before $L is used)
        script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
        for var in ("T", "L"):
            set_at = script.find("\n%s=$APPDIR/" % var)
            used_at = script.find('"$%s/' % var)
            self.assertGreaterEqual(set_at, 0, var + " is never set")
            self.assertLess(set_at, used_at, var + " is used before it is set")

    @unittest.skipUnless(shutil.which("sh"), "AppRun is a POSIX shell script")
    def test_apprun_points_into_the_image(self):
        # (2026-11-14: the merge that took out the prebuilt renderer took out AppRun's `LIB=` too, and v2.3.0's AppImage
        # exported EMSDK=/emsdk and HOOKSHOT_SYSROOT=/sysroot: every Linux player's build fell back to the system's clang with no
        # sysroot and stopped at "'stdio.h' file not found". AppRun's set-up runs here under -u, as the image runs it)
        apprun = (ROOT / "builder/linux/AppRun").read_text(encoding="utf-8")
        head = apprun[:apprun.index("\nPY=")]
        temp = tempfile.TemporaryDirectory(prefix="hookshot-apprun-")
        self.addCleanup(temp.cleanup)
        (Path(temp.name) / "AppRun").write_text("")
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin")}
        # (the folder as the shell spells it, with links resolved as readlink -f does: MSYS's spelling on Windows)
        here = subprocess.run(["sh", "-c", 'cd "$1" && pwd -P', "sh", temp.name], env=env, capture_output=True,
                              text=True).stdout.strip()
        r = subprocess.run(["sh", "-u", "-c", head + '\nenv\n', here + "/AppRun"], env=env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        got = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)
        lib = here + "/usr/lib/hookshot"
        for var, want in (("HOOKSHOT_TOOLS", here + "/usr/share/hookshot"), ("EMSDK", lib + "/emsdk"),
                          ("HOOKSHOT_SYSROOT", lib + "/sysroot"), ("HOOKSHOT_FFMPEG", lib + "/ffmpeg"),
                          ("HOOKSHOT_SDL2", lib + "/libSDL2-2.0.so.0")):
            self.assertEqual(got.get(var), want, var)

    def test_the_image_makes_the_third_party_objects(self):
        # (2026-11-15: a merge dropped appimage.sh's one line that makes the set, and v2.3.0's and v2.3.1's first images made
        # every Linux player's first build compile the third-party code again; check_player_build.sh caught it on the image.
        # The set is compiled against the image's sysroot, so after it)
        script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
        make = script.find('nx2/third_party_objects.py make "$T/third_party_objects"')
        self.assertGreater(make, 0, "appimage.sh makes no third-party object set")
        self.assertLess(script.index("\nS=$L/sysroot"), make)
        self.assertIn("HOOKSHOT_SYSROOT=$S ", script[script.rindex("\n", 0, make):make])

    def test_no_prebuilt_renderer(self):
        # (2026-11-14: every build compiles the Vulkan back end, as with DLSS and FSR 4: one way to build it. Nothing sets or
        # reads HOOKSHOT_VK_PREBUILT, and appimage.sh builds no renderer and copies none into the image)
        for folder, suffixes in (("nx2", (".py",)), ("scripts", (".py", ".sh")), ("builder", (".py", ".sh", ".cs"))):
            for f in (ROOT / folder).rglob("*"):
                if f.is_file() and f.suffix in suffixes:
                    self.assertNotIn("HOOKSHOT_VK_PREBUILT", f.read_text(encoding="utf-8", errors="replace"), f)
        apprun = (ROOT / "builder/linux/AppRun").read_text(encoding="utf-8")
        self.assertNotIn("HOOKSHOT_VK_PREBUILT", apprun)
        self.assertNotIn("hookshot_vk.so", apprun)
        script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
        self.assertNotIn("RX_VK_ONLY", script)
        self.assertNotIn('"$L/hookshot_vk.so"', script)
        build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
        self.assertNotIn("prebuilt_renderer", build)
        self.assertIn("build_vk = a.standalone and vk_headers and spirv_headers and vma_headers", build)

    def test_the_test_sets_what_apprun_sets(self):
        apprun = (ROOT / "builder/linux/AppRun").read_text(encoding="utf-8")
        exported = set(re.findall(r"^export (HOOKSHOT_\w+|EMSDK)=", apprun, re.MULTILINE))
        source = Path(__file__).read_text(encoding="utf-8")
        used = set(re.findall(r'"(HOOKSHOT_\w+|EMSDK)":', source))
        self.assertEqual(exported - used - {"HOOKSHOT_SDL2", "HOOKSHOT_BUILDER_EXE"}, set())   # (those two: the game's, after the build)


if __name__ == "__main__":
    unittest.main()
