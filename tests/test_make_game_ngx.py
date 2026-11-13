"""nx2/make_game.py and NVIDIA DLSS: the runtime placed in the game folder's ngx folder (never beside the exe), and the
fetch the Hookshot Builder's DLSS tick asks for, which needs NVIDIA's license accepted. No network, no game.

Run with: python -I tests/test_make_game_ngx.py
"""
import importlib.util
import io
import os
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_ngx_under_test", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class NgxRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-make-game-ngx-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"
        self.out = self.root / "game"
        (self.out / "bin").mkdir(parents=True)
        self.ngx = make_game.ngx_script()

    def sdk_files(self, platform, runtime=True, framegen=True):
        lib = Path(self.ngx.library(str(self.sdk), platform))
        lib.parent.mkdir(parents=True, exist_ok=True)
        lib.write_bytes(b"lib")
        if runtime:
            rt = Path(self.ngx.runtime(str(self.sdk), platform))
            rt.parent.mkdir(parents=True, exist_ok=True)
            rt.write_bytes(b"runtime")
            if framegen:
                Path(self.ngx.runtime(str(self.sdk), platform, "framegen")).write_bytes(b"framegen")

    def place(self, platform, sdk=None, said=None):
        env = {"HOOKSHOT_NGX_SDK": str(self.sdk) if sdk is None else sdk}
        with mock.patch.object(make_game, "LINUX", platform == "linux"), mock.patch.dict(os.environ, env), \
                mock.patch("sys.stdout", io.StringIO()) as out:
            dst = make_game.place_ngx_runtime(str(self.out))
        if said is not None:
            said.append(out.getvalue())
        return dst

    def test_windows_runtime_goes_to_the_ngx_folder(self):
        self.sdk_files("windows")
        dst = self.place("windows")
        self.assertEqual(Path(dst), self.out / "ngx" / "nvngx_dlss.dll")
        self.assertEqual(Path(dst).read_bytes(), b"runtime")
        # (DLSS Frame Generation's runtime beside it, where NGX looks before the driver's own copy)
        self.assertEqual((self.out / "ngx" / "nvngx_dlssg.dll").read_bytes(), b"framegen")
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["bin", "ngx"])
        self.assertEqual(list((self.out / "bin").iterdir()), [])   # (nothing beside the exe)
        self.assertFalse([p for p in self.out.rglob("*") if p.name.lower() == "_nvngx.dll"])

    def built(self, platform, sdk=None):
        env = {"HOOKSHOT_NGX_SDK": str(self.sdk) if sdk is None else sdk}
        with mock.patch.object(make_game, "LINUX", platform == "linux"), mock.patch.dict(os.environ, env):
            return make_game.ngx_built()

    def test_built_with_this_platforms_library(self):
        # (the camera jitter is no longer tied to this: nx2/oot_jitter.py is in every standalone lift, for FSR 3.1 as well)
        self.assertFalse(self.built("linux", sdk=""))
        self.assertFalse(self.built("linux"))   # (no library yet)
        self.sdk_files("linux", runtime=False)
        self.assertTrue(self.built("linux"))
        self.assertFalse(self.built("windows"))   # (another platform's library only)

    def test_fsr_build(self):
        """--fsr: HOOKSHOT_FSR=1 for nx2/build.py; an --update of that folder keeps it (its mark), --no-fsr takes it out. (The
        camera jitter FSR 4 needs is in every build: nx2/lift.py)"""
        out = str(self.out)
        mark = self.out / make_game.FSR_MARK
        plain = SimpleNamespace(fsr=False, no_fsr=False, update=None)
        self.assertFalse(make_game.fsr_wanted(plain, out))
        self.assertTrue(make_game.fsr_wanted(SimpleNamespace(fsr=True, no_fsr=False, update=None), out))
        update = SimpleNamespace(fsr=False, no_fsr=False, update=out)
        self.assertFalse(make_game.fsr_wanted(update, out))
        mark.write_text("")
        self.assertTrue(make_game.fsr_wanted(update, out))
        self.assertFalse(make_game.fsr_wanted(SimpleNamespace(fsr=False, no_fsr=True, update=out), out))
        self.assertFalse(make_game.fsr_wanted(plain, out))   # (a new build without --fsr: the mark is for --update only)

    def test_linux_runtime(self):
        self.sdk_files("linux")
        self.assertEqual(Path(self.place("linux")), self.out / "ngx" / "libnvidia-ngx-dlss.so.310.9.16")
        self.assertTrue((self.out / "ngx" / "libnvidia-ngx-dlssg.so.310.9.16").is_file())

    def test_rebuilding_without_dlss_removes_stale_runtime(self):
        self.sdk_files("linux")
        self.assertTrue((Path(self.place("linux"))).is_file())
        self.assertIsNone(self.place("linux", sdk=""))
        self.assertFalse((self.out / "ngx").exists())   # (both runtimes gone, and the folder)

    def test_an_older_sdk_folder_says_how_to_get_frame_generations_runtime(self):
        self.sdk_files("windows", framegen=False)
        said = []
        self.assertEqual(Path(self.place("windows", said=said)), self.out / "ngx" / "nvngx_dlss.dll")
        self.assertEqual([p.name for p in (self.out / "ngx").iterdir()], ["nvngx_dlss.dll"])
        self.assertIn("has no nvngx_dlssg.dll yet", said[0])
        self.assertIn("scripts/ngx_sdk.py", said[0])

    def test_nothing_without_the_sdk_or_its_platform(self):
        self.assertIsNone(self.place("windows", sdk=""))
        self.sdk_files("linux")
        self.assertIsNone(self.place("windows"))   # (a Linux SDK folder: no Windows library, no DLSS in this build)
        self.sdk_files("windows", runtime=False)
        self.assertIsNone(self.place("windows"))
        self.assertFalse((self.out / "ngx").exists())


class FetchGateTests(unittest.TestCase):
    def fetch(self, accept, sdk="sdk"):
        calls = []
        ngx = SimpleNamespace(NOTICE="notice", fetch=lambda *a, **k: calls.append(a))
        with mock.patch.object(make_game, "ngx_script", return_value=ngx), mock.patch.dict(os.environ, {"HOOKSHOT_NGX_SDK": sdk}), \
                mock.patch("sys.stdout", io.StringIO()) as out:
            try:
                make_game.fetch_ngx_sdk(SimpleNamespace(accept_nvidia_license=accept))
            except SystemExit as e:
                return calls, e.code, out.getvalue()
        return calls, 0, out.getvalue()

    def test_needs_the_license_accepted(self):
        calls, code, out = self.fetch(False)
        self.assertEqual((calls, code), ([], 1))
        self.assertIn("@@error", out)
        self.assertIn("license", out)

    def test_needs_the_sdk_folder(self):
        calls, code, _ = self.fetch(True, sdk="")
        self.assertEqual((calls, code), ([], 1))

    def test_fetches_this_platforms_files_after_the_notice(self):
        calls, code, out = self.fetch(True)
        self.assertEqual(code, 0)
        self.assertEqual(calls, [("sdk", make_game.ngx_platform())])
        self.assertTrue(out.startswith("notice"))

    def test_flags_are_off_by_default(self):
        with mock.patch("sys.argv", ["make_game.py", "--out", "x", "--nup", "y"]):
            a = make_game.parse_args()
        self.assertFalse(a.fetch_ngx_sdk)
        self.assertFalse(a.accept_nvidia_license)


class NoScriptTests(unittest.TestCase):
    """The AppImage carries no scripts/ (scripts/linux/appimage.sh): with HOOKSHOT_NGX_SDK set there, ngx_script() says so and
    stops, not a FileNotFoundError traceback from importlib"""

    def test_missing_script_is_said_plainly(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-no-scripts-") as root, mock.patch.object(make_game, "ROOT", root), \
                mock.patch.dict(os.environ, {"HOOKSHOT_NGX_SDK": os.path.join(root, "sdk")}), \
                mock.patch("sys.stdout", io.StringIO()) as out:
            with self.assertRaises(SystemExit) as stopped:
                make_game.place_ngx_runtime(os.path.join(root, "game"))
        self.assertEqual(stopped.exception.code, 1)
        self.assertIn("@@error", out.getvalue())
        self.assertIn("ngx_sdk.py", out.getvalue())
        self.assertIn("Unset HOOKSHOT_NGX_SDK", out.getvalue())


def load_linux_builder():
    import sys
    import types
    for name in ("tkinter", "tkinter.filedialog", "tkinter.messagebox", "tkinter.ttk"):
        if name not in sys.modules:
            sys.modules[name] = types.ModuleType(name)
    tk = sys.modules["tkinter"]
    tk.Tk = type("Tk", (), {})
    tk.filedialog = sys.modules["tkinter.filedialog"]
    tk.messagebox = sys.modules["tkinter.messagebox"]
    tk.ttk = sys.modules["tkinter.ttk"]
    spec = importlib.util.spec_from_file_location("hookshot_builder_dlss", ROOT / "builder" / "linux" / "hookshot_builder.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeVar:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class LinuxBuilderDlssTests(unittest.TestCase):
    def setUp(self):
        self.builder = load_linux_builder()
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-linux-dlss-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_license_prompt_and_env(self):
        self.assertIn("https://github.com/NVIDIA/DLSS/blob/", self.builder.DLSS_LICENSE)
        self.assertIn("never share a game folder built with them", self.builder.DLSS_LICENSE)
        with mock.patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.root)}):
            sdk = str(self.root / "hookshot-builder" / "ngx-sdk")   # (the builder's cache folder, not the game's data folder)
            self.assertEqual(self.builder.ngx_sdk_dir(), sdk)
            env_off = self.builder.make_game_env(False, {"HOOKSHOT_NGX_SDK": "/stale", "HOOKSHOT_VK_PREBUILT": "/app/hookshot_vk.so"})
            self.assertNotIn("HOOKSHOT_NGX_SDK", env_off)
            self.assertEqual(env_off["HOOKSHOT_VK_PREBUILT"], "/app/hookshot_vk.so")
            self.assertEqual(self.builder.dlss_args(False), [])
            env_on = self.builder.make_game_env(True, {"HOOKSHOT_VK_PREBUILT": "/app/hookshot_vk.so"})
            self.assertEqual(env_on["HOOKSHOT_NGX_SDK"], sdk)
            self.assertNotIn("HOOKSHOT_VK_PREBUILT", env_on)
            self.assertEqual(self.builder.dlss_args(True), ["--fetch-ngx-sdk", "--accept-nvidia-license"])

    def test_declining_license_turns_dlss_back_off(self):
        saved = {}
        win = SimpleNamespace(dlss=FakeVar(True), dlss_toggled=False, cfg={}, apply_mode=lambda: None)
        with mock.patch.object(self.builder.messagebox, "askyesno", return_value=False, create=True), \
                mock.patch.object(self.builder, "save_config", lambda cfg: saved.update(cfg)):
            self.builder.Builder.on_dlss_changed(win)
        self.assertFalse(win.dlss.get())
        self.assertFalse(win.dlss_toggled)
        self.assertEqual(saved.get("dlss"), "0")
        self.assertNotIn("nvidia_license", saved)

    def test_accepting_license_saves_and_marks_toggled(self):
        saved = {}
        win = SimpleNamespace(dlss=FakeVar(True), dlss_toggled=False, cfg={}, apply_mode=lambda: None)
        with mock.patch.object(self.builder.messagebox, "askyesno", return_value=True, create=True), \
                mock.patch.object(self.builder, "save_config", lambda cfg: saved.update(cfg)):
            self.builder.Builder.on_dlss_changed(win)
        self.assertTrue(win.dlss.get())
        self.assertTrue(win.dlss_toggled)
        self.assertEqual(saved.get("dlss"), "1")
        self.assertEqual(saved.get("nvidia_license"), "1")   # (in the settings: the SDK's folder is a cache)

    def test_folder_has_dlss_needs_runtime_and_accepted_license(self):
        game = self.root / "game"
        (game / "ngx").mkdir(parents=True)
        config = self.root / "config" / "builder.ini"
        with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(self.root / "data"), "XDG_CACHE_HOME": str(self.root / "cache")}), \
                mock.patch.object(self.builder, "CONFIG", str(config)):
            self.assertFalse(self.builder.folder_has_dlss(str(game)))
            (game / "ngx" / "libnvidia-ngx-dlss.so.310.9.16").write_bytes(b"rt")
            self.assertFalse(self.builder.folder_has_dlss(str(game)))
            # (the SDK in the cache says nothing: the cache can be emptied, and the license was accepted or not all the same)
            lic = Path(self.builder.ngx_sdk_dir()) / "LICENSE.txt"
            lic.parent.mkdir(parents=True, exist_ok=True)
            lic.write_text("NVIDIA RTX SDKs license", encoding="utf-8")
            self.assertFalse(self.builder.folder_has_dlss(str(game)))
            self.builder.save_config({"nvidia_license": "1"})
            self.assertTrue(self.builder.folder_has_dlss(str(game)))
            shutil.rmtree(Path(self.builder.ngx_sdk_dir()))   # (the cache emptied: an update keeps DLSS, and fetches the SDK again)
            self.assertTrue(self.builder.folder_has_dlss(str(game)))

    def test_old_sdk_moves_to_the_cache_and_its_license_is_recorded(self):
        config = self.root / "config" / "builder.ini"
        with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(self.root / "data"), "XDG_CACHE_HOME": str(self.root / "cache")}), \
                mock.patch.object(self.builder, "CONFIG", str(config)):
            old = Path(self.builder.old_ngx_sdk_dir())
            self.assertEqual(old, self.root / "data" / "hookshot" / "builder" / "ngx-sdk")
            self.builder.move_old_ngx_sdk()   # (nothing there: nothing done, nothing recorded)
            self.assertFalse(config.exists())
            (old / "runtime").mkdir(parents=True)
            (old / "LICENSE.txt").write_text("NVIDIA RTX SDKs license", encoding="utf-8")
            (old / "runtime" / "libnvidia-ngx-dlss.so.310.9.16").write_bytes(b"rt")
            self.builder.save_config({"out": "/games/hookshot"})
            self.assertTrue(self.builder.license_accepted())   # (accepted before the record: the old SDK says so)
            self.builder.move_old_ngx_sdk()
            new = Path(self.builder.ngx_sdk_dir())
            self.assertFalse(old.exists())
            self.assertEqual((new / "runtime" / "libnvidia-ngx-dlss.so.310.9.16").read_bytes(), b"rt")
            self.assertEqual(self.builder.load_config(), {"out": "/games/hookshot", "nvidia_license": "1"})
            self.assertTrue(self.builder.license_accepted())
            # (an SDK in both places: the cache's is kept, the old one goes)
            (old).mkdir(parents=True)
            (old / "LICENSE.txt").write_text("old", encoding="utf-8")
            self.builder.move_old_ngx_sdk()
            self.assertFalse(old.exists())
            self.assertEqual((new / "LICENSE.txt").read_text(encoding="utf-8"), "NVIDIA RTX SDKs license")

    def test_toggling_dlss_on_up_to_date_folder_offers_update(self):
        game = self.root / "game"
        (game / ".hookshot_exefs").mkdir(parents=True)
        (game / "romfs").mkdir(parents=True)
        (game / "hookshot_version.txt").write_text("version=v2.0.12\n", encoding="utf-8")
        btn = SimpleNamespace(winfo_exists=lambda: True, text="", configure=lambda **kw: setattr(btn, "text", kw.get("text", "")))
        win = SimpleNamespace(launch_folder=None, build_button=btn, out=FakeVar(str(game)), tools="v2.0.12", latest="v2.0.12",
                              source_picked=False, dlss_toggled=True, dlss=FakeVar(True), status=FakeVar(""), mode="build",
                              switching=lambda: False)
        with mock.patch.dict(os.environ, {"HOOKSHOT_BUILDER_HOME": str(self.root)}):
            self.builder.Builder.apply_mode(win)
        self.assertEqual(win.mode, "update-game")
        self.assertEqual(btn.text, "Update")
        self.assertIn("with NVIDIA DLSS", win.status.get())

    def test_build_and_appimage_package_dlss_sources(self):
        build_py = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        self.assertIn('return "" if NGX_SDK else os.environ.get("HOOKSHOT_VK_PREBUILT", "")', build_py)
        self.assertIn('vk_prebuilt = prebuilt_renderer()', build_py)
        appimage = (ROOT / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8")
        for piece in ("cp scripts/ngx_sdk.py", "cp -a gpu/vk gpu/compat gpu/third_party", "third_party/imgui", "libstdc++.a"):
            self.assertIn(piece, appimage)


if __name__ == "__main__":
    unittest.main()
