"""scripts/ffx_sdk.py: its file table, the SHA-256 checks, the retries, the copy into a game folder and AMD's license accepted
first (here, in make_game.py and in the Windows Builder), without the network.

Run with: python -I tests/test_ffx_sdk.py
"""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ffx_sdk_under_test", ROOT / "scripts" / "ffx_sdk.py")
ffx_sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ffx_sdk)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Opener:
    """urlopen over a dict of path -> bytes; records what was asked for. fail: paths that raise OSError once each"""
    def __init__(self, contents, fail=()):
        self.contents, self.asked, self.fail = contents, [], set(fail)

    def __call__(self, url, timeout=None):
        if timeout != ffx_sdk.TIMEOUT:
            raise AssertionError("urlopen without the timeout (%r)" % (timeout,))
        path = url[len(ffx_sdk.BASE) + 1:]
        self.asked.append(path)
        if path in self.fail:
            self.fail.discard(path)
            raise OSError("connection reset")
        return io.BytesIO(self.contents[path])


def fake_table(data):
    """FILES with each file's SHA-256 that of data[path]"""
    return [(p, digest(data[p]), name) for p, _, name in ffx_sdk.FILES]


class TableTests(unittest.TestCase):
    def test_pinned_release(self):
        self.assertRegex(ffx_sdk.COMMIT, r"^[0-9a-f]{40}$")
        self.assertIn(ffx_sdk.COMMIT, ffx_sdk.BASE)

    def test_files(self):
        names = [name for _, _, name in ffx_sdk.FILES]
        self.assertEqual(names, ["amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll", "LICENSE-AMD-FSR-SDK.md"])
        for path, sha, _ in ffx_sdk.FILES:
            self.assertTrue(path.startswith("Kits/FidelityFX/"), path)
            self.assertRegex(sha, r"^[0-9a-f]{64}$")


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.data = {p: ("file %s" % p).encode() for p, _, _ in ffx_sdk.FILES}

    def test_fetch_and_install(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ffx_sdk, "FILES", fake_table(self.data)):
            opener = Opener(self.data)
            said = []
            ffx_sdk.fetch(os.path.join(tmp, "sdk"), opener=opener, say=said.append)
            self.assertEqual(len(said), 3)
            self.assertTrue(ffx_sdk.complete(os.path.join(tmp, "sdk")))
            ffx_sdk.fetch(os.path.join(tmp, "sdk"), opener=opener, say=said.append)   # (there already: nothing asked)
            self.assertEqual(len(opener.asked), 3)
            game_ffx = ffx_sdk.install(os.path.join(tmp, "sdk"), os.path.join(tmp, "game", "ffx"))
            self.assertTrue(ffx_sdk.complete(game_ffx))

    def test_wrong_file_not_kept(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ffx_sdk, "FILES", fake_table(self.data)):
            bad = dict(self.data)
            first = ffx_sdk.FILES[0][0]
            bad[first] = b"something else"
            with self.assertRaises(ValueError):
                ffx_sdk.fetch(tmp, opener=Opener(bad), say=lambda _: None)
            self.assertFalse(os.path.exists(os.path.join(tmp, ffx_sdk.FILES[0][2])))
            self.assertFalse(os.path.exists(os.path.join(tmp, ffx_sdk.FILES[0][2] + ".part")))
            self.assertFalse(ffx_sdk.complete(tmp))

    def test_players_dlls_left_alone(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ffx_sdk, "FILES", fake_table(self.data)), \
                mock.patch.object(ffx_sdk, "DLLS", [n for _, _, n in fake_table(self.data) if n.endswith(".dll")]):
            ffx_sdk.fetch(os.path.join(tmp, "sdk"), opener=Opener(self.data), say=lambda _: None)
            game = os.path.join(tmp, "game")
            os.makedirs(game)
            theirs = os.path.join(game, ffx_sdk.DLLS[1])
            with open(theirs, "wb") as f:
                f.write(b"a newer SDK's upscaler")
            said = []
            ffx_sdk.install(os.path.join(tmp, "sdk"), game, say=said.append)
            with open(theirs, "rb") as f:
                self.assertEqual(f.read(), b"a newer SDK's upscaler")
            self.assertFalse(os.path.exists(os.path.join(game, ffx_sdk.DLLS[0])))   # (not mixed with v2.4.0's loader)
            self.assertEqual(len(said), 1)
            self.assertIn(ffx_sdk.DLLS[1], said[0])

    def test_retry_after_a_reset(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ffx_sdk, "FILES", fake_table(self.data)):
            second = ffx_sdk.FILES[1][0]
            opener = Opener(self.data, fail=[second])
            ffx_sdk.fetch(tmp, opener=opener, say=lambda _: None)
            self.assertEqual(opener.asked.count(second), 2)
            self.assertTrue(ffx_sdk.complete(tmp))


class MakeGameTests(unittest.TestCase):
    """nx2/make_game.py --fetch-fsr-runtime: AMD's runtime goes into the game folder only once it was fetched whole"""
    def test_place_fsr_runtime(self):
        spec = importlib.util.spec_from_file_location("make_game_ffx_under_test", ROOT / "nx2" / "make_game.py")
        make_game = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(make_game)
        data = {p: ("file %s" % p).encode() for p, _, _ in ffx_sdk.FILES}
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ffx_sdk, "FILES", fake_table(data)), \
                mock.patch.object(make_game, "ffx_script", return_value=ffx_sdk), \
                mock.patch.dict(os.environ, {"HOOKSHOT_FFX_SDK": os.path.join(tmp, "sdk")}):
            game = os.path.join(tmp, "game")
            os.makedirs(game)
            self.assertIsNone(make_game.place_fsr_runtime(game))   # (nothing fetched: nothing placed)
            ffx_sdk.fetch(os.path.join(tmp, "sdk"), opener=Opener(data), say=lambda _: None)
            placed = make_game.place_fsr_runtime(game)
            if make_game.LINUX:
                self.assertIsNone(placed)   # (AMD's runtime is Windows' alone)
            else:
                self.assertEqual(placed, game)
                for _, _, name in ffx_sdk.FILES:
                    self.assertTrue(os.path.isfile(os.path.join(game, name)), name)


def load_make_game():
    spec = importlib.util.spec_from_file_location("make_game_ffx_license_under_test", ROOT / "nx2" / "make_game.py")
    make_game = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(make_game)
    return make_game


class LicenseTests(unittest.TestCase):
    """AMD's license accepted before anything of AMD's is downloaded: --accept-amd-license, as NVIDIA's for DLSS"""
    def test_accepted(self):
        self.assertTrue(ffx_sdk.accepted(True, interactive=False))
        self.assertFalse(ffx_sdk.accepted(False, interactive=False))   # (no one to ask: no)
        self.assertTrue(ffx_sdk.accepted(False, ask=lambda _: " Yes ", interactive=True))
        self.assertFalse(ffx_sdk.accepted(False, ask=lambda _: "no", interactive=True))

        def eof(_):
            raise EOFError
        self.assertFalse(ffx_sdk.accepted(False, ask=eof, interactive=True))

    def test_script_downloads_nothing_unaccepted(self):
        with mock.patch.object(ffx_sdk, "fetch", side_effect=AssertionError("downloaded without the license")), \
                mock.patch.object(ffx_sdk, "accepted", return_value=False), \
                mock.patch("sys.stdout", io.StringIO()), mock.patch("sys.stderr", io.StringIO()) as err:
            self.assertEqual(ffx_sdk.main(["--game", "unused"]), 2)
        self.assertIn("--accept-amd-license", err.getvalue())

    def test_make_game_fails_before_any_download(self):
        make_game = load_make_game()
        ffx = mock.Mock()
        with mock.patch.object(make_game, "ffx_script", return_value=ffx), \
                mock.patch.object(make_game, "say") as said, self.assertRaises(SystemExit):
            make_game.fetch_fsr_runtime(SimpleNamespace(fetch_fsr_runtime=True, accept_amd_license=False))
        ffx.fetch.assert_not_called()
        self.assertIn("AMD FSR 4 needs AMD's license accepted first (--accept-amd-license).", str(said.call_args))

    def test_make_game_fetches_once_accepted(self):
        make_game = load_make_game()
        ffx = mock.Mock(NOTICE="notice")
        with mock.patch.object(make_game, "ffx_script", return_value=ffx), mock.patch("sys.stdout", io.StringIO()):
            make_game.fetch_fsr_runtime(SimpleNamespace(fetch_fsr_runtime=True, accept_amd_license=True))
        ffx.fetch.assert_called_once()


class BuilderTests(unittest.TestCase):
    """The Windows Hookshot Builder: the AMD FSR 4 tick shows AMD's license, stays only on yes, and only then adds the flag
    pair (as the NVIDIA DLSS tick does with NVIDIA's)"""
    def setUp(self):
        folder = ROOT / "builder" / "HookshotBuilder"
        self.code = (folder / "MainWindow.xaml.cs").read_text(encoding="utf-8")
        self.xaml = (folder / "MainWindow.xaml").read_text(encoding="utf-8")

    def test_tick_shows_the_license(self):
        self.assertRegex(self.xaml, r'x:Name="FsrTick"[^>]*Click="FsrChanged"')
        start = self.code.index("void FsrChanged(")
        body = self.code[start:self.code.index("\n    }", start)]
        self.assertIn("MessageBox.Show(this, FsrLicense", body)
        self.assertRegex(body, r"fsrLicense = MessageBox\.Show\(this, FsrLicense[^;]*==\s*MessageBoxResult\.Yes;")
        self.assertIn("if (!fsrLicense) FsrTick.IsChecked = false;", body)
        start = self.code.index("const string FsrLicense")
        text = self.code[start:self.code.index(";", start)]
        self.assertIn(ffx_sdk.LICENSE_URL, text)
        self.assertIn("Do you accept AMD's license?", text)

    def test_flags_only_with_the_tick(self):
        pair = '"--fetch-fsr-runtime", "--accept-amd-license"'
        self.assertEqual(self.code.count(pair), 1)
        line = next(l for l in self.code.splitlines() if pair in l)
        self.assertTrue(line.strip().startswith("if (fsr && "), line)
        self.assertIn("fsr: FsrTick.IsChecked == true && fsrLicense", self.code)   # (Build: the tick, AMD's license accepted)
        self.assertIn("fsr ? Storage.FfxSdk : null", self.code)      # (AMD's runtime kept in the builder's folder)


if __name__ == "__main__":
    unittest.main()
