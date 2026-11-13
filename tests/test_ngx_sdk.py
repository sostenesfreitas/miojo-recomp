"""scripts/ngx_sdk.py: its per-platform file table, the SHA-256 checks and NVIDIA's license gate, without the network.

Run with: python -I tests/test_ngx_sdk.py
"""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ngx_sdk_under_test", ROOT / "scripts" / "ngx_sdk.py")
ngx_sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ngx_sdk)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Opener:
    """urlopen over a dict of path -> bytes; records what was asked for"""
    def __init__(self, contents):
        self.contents, self.asked = contents, []

    def __call__(self, url, timeout=None):
        if timeout != ngx_sdk.TIMEOUT:
            raise AssertionError("urlopen without the timeout (%r)" % (timeout,))
        path = url[len(ngx_sdk.BASE) + 1:]
        self.asked.append(path)
        return io.BytesIO(self.contents[path])


class TableTests(unittest.TestCase):
    def test_pinned_release(self):
        self.assertRegex(ngx_sdk.COMMIT, r"^[0-9a-f]{40}$")
        self.assertTrue(ngx_sdk.BASE.startswith("https://raw.githubusercontent.com/NVIDIA/DLSS/" + ngx_sdk.COMMIT))

    def test_every_file_has_a_sha256(self):
        for platform in ngx_sdk.PLATFORMS:
            for path, sha, _ in ngx_sdk.files(platform):
                self.assertRegex(sha, r"^[0-9a-f]{64}$", path)

    def test_windows_files(self):
        table = {rel: (path, sha) for path, sha, rel in ngx_sdk.files("windows")}
        self.assertEqual(table["lib/nvsdk_ngx_s.lib"],
                         ("lib/Windows_x86_64/x64/nvsdk_ngx_s.lib",
                          "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"))
        self.assertEqual(table["runtime/nvngx_dlss.dll"][0], "lib/Windows_x86_64/rel/nvngx_dlss.dll")
        # (DLSS Frame Generation's runtime beside it: without it NGX took the driver's own, which on an RTX 5080 was 310.2.1,
        # 1 made frame at most and every set refused at 2x)
        self.assertEqual(table["runtime/nvngx_dlssg.dll"],
                         ("lib/Windows_x86_64/rel/nvngx_dlssg.dll",
                          "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"))
        self.assertIn("LICENSE.txt", table)
        self.assertIn("include/nvsdk_ngx_vk.h", table)
        self.assertIn("include/nvsdk_ngx_helpers_vk.h", table)
        # (nothing of Linux's, and never the NGX core: _nvngx.dll comes with the driver and must not be beside the game)
        self.assertFalse([r for r in table if "nvngx.dll" in r and not r.endswith("nvngx_dlss.dll")])
        self.assertFalse([p for p, _ in table.values() if "Linux" in p])

    def test_linux_files(self):
        table = {rel: path for path, _, rel in ngx_sdk.files("linux")}
        self.assertEqual(table["lib/libnvsdk_ngx.a"], "lib/Linux_x86_64/libnvsdk_ngx.a")
        self.assertEqual(table["runtime/libnvidia-ngx-dlss.so.310.9.16"], "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16")
        self.assertEqual(table["runtime/libnvidia-ngx-dlssg.so.310.9.16"],
                         "lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16")
        self.assertFalse([p for p in table.values() if "Windows" in p])

    def test_build_only_leaves_out_the_runtimes(self):
        for platform in ngx_sdk.PLATFORMS:
            full = {rel for _, _, rel in ngx_sdk.files(platform)}
            build = {rel for _, _, rel in ngx_sdk.files(platform, runtime=False)}
            self.assertEqual(full - build, {"runtime/" + ngx_sdk.runtime_name(platform, k) for k in ngx_sdk.RUNTIMES})
            self.assertEqual(len(full - build), 2)   # (DLSS's and DLSS Frame Generation's)

    def test_platforms_share_the_headers_and_one_folder(self):
        linux = {rel for _, _, rel in ngx_sdk.files("linux")}
        windows = {rel for _, _, rel in ngx_sdk.files("windows")}
        self.assertEqual({r for r in linux if not r.startswith(("lib/", "runtime/"))},
                         {r for r in windows if not r.startswith(("lib/", "runtime/"))})
        self.assertEqual(len(linux & windows), len(ngx_sdk.COMMON))   # (both platforms' libraries fit in one SDK folder)

    def test_library_and_runtime_paths(self):
        sdk = os.path.join("x", "sdk")
        self.assertEqual(ngx_sdk.library(sdk, "windows"), os.path.join(sdk, "lib", "nvsdk_ngx_s.lib"))
        self.assertEqual(ngx_sdk.library(sdk, "linux"), os.path.join(sdk, "lib", "libnvsdk_ngx.a"))
        self.assertEqual(ngx_sdk.runtime(sdk, "windows"), os.path.join(sdk, "runtime", "nvngx_dlss.dll"))
        self.assertEqual(ngx_sdk.runtime_name("windows"), "nvngx_dlss.dll")
        self.assertEqual(ngx_sdk.runtime(sdk, "windows", "framegen"), os.path.join(sdk, "runtime", "nvngx_dlssg.dll"))
        self.assertEqual(ngx_sdk.runtime_name("linux", "framegen"), "libnvidia-ngx-dlssg.so.310.9.16")

    def test_host_platform(self):
        with mock.patch.dict(os.environ, {"RX_TARGET": "windows"}):
            self.assertEqual(ngx_sdk.host_platform(), "windows")
        if os.name != "nt":
            with mock.patch.dict(os.environ, {"RX_TARGET": ""}):
                self.assertEqual(ngx_sdk.host_platform(), "linux")


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-ngx-sdk-")
        self.addCleanup(self.temp.cleanup)
        self.sdk = os.path.join(self.temp.name, "sdk")
        self.good = {"lib/Windows_x86_64/x64/nvsdk_ngx_s.lib": b"lib", "LICENSE.txt": b"license",
                     "lib/Windows_x86_64/rel/nvngx_dlss.dll": b"runtime"}
        self.table = [(p, digest(d), ngx_sdk.destination(p)) for p, d in self.good.items()]

    def test_fetch_checks_and_skips_what_is_there(self):
        opener = Opener(self.good)
        with mock.patch.object(ngx_sdk, "files", lambda platform, runtime=True: self.table):
            ngx_sdk.fetch(self.sdk, "windows", opener, say=lambda *_: None)
            self.assertEqual(sorted(opener.asked), sorted(self.good))
            self.assertEqual(Path(ngx_sdk.library(self.sdk, "windows")).read_bytes(), b"lib")
            opener.asked.clear()
            ngx_sdk.fetch(self.sdk, "windows", opener, say=lambda *_: None)   # (all there and right: nothing downloaded)
            self.assertEqual(opener.asked, [])

    def test_wrong_download_is_not_kept(self):
        bad = dict(self.good, **{"lib/Windows_x86_64/x64/nvsdk_ngx_s.lib": b"not nvidia's"})
        with mock.patch.object(ngx_sdk, "files", lambda platform, runtime=True: self.table):
            with self.assertRaisesRegex(ValueError, "nvsdk_ngx_s.lib is not the file expected"):
                ngx_sdk.fetch(self.sdk, "windows", Opener(bad), say=lambda *_: None)
        lib = ngx_sdk.library(self.sdk, "windows")
        self.assertFalse(os.path.exists(lib))
        self.assertFalse(os.path.exists(lib + ".part"))

    def test_wrong_file_there_is_downloaded_again(self):
        lib = ngx_sdk.library(self.sdk, "windows")
        os.makedirs(os.path.dirname(lib))
        Path(lib).write_bytes(b"old")
        path = "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
        self.assertTrue(ngx_sdk.fetch_one(path, digest(b"lib"), lib, Opener(self.good)))
        self.assertEqual(Path(lib).read_bytes(), b"lib")

    def test_runtime_into_the_game_folder(self):
        rt = ngx_sdk.runtime(self.sdk, "windows")
        os.makedirs(os.path.dirname(rt))
        Path(rt).write_bytes(b"runtime")
        Path(ngx_sdk.runtime(self.sdk, "windows", "framegen")).write_bytes(b"framegen")
        game = os.path.join(self.temp.name, "game")
        dst = ngx_sdk.install_runtime(self.sdk, "windows", os.path.join(game, "ngx"))
        self.assertEqual(dst, os.path.join(game, "ngx", "nvngx_dlss.dll"))
        self.assertEqual(Path(dst).read_bytes(), b"runtime")
        self.assertEqual(Path(game, "ngx", "nvngx_dlssg.dll").read_bytes(), b"framegen")   # (beside it: where NGX looks)
        self.assertEqual(os.listdir(game), ["ngx"])   # (nothing beside the exe)
        self.assertEqual(sorted(os.listdir(os.path.join(game, "ngx"))), ["nvngx_dlss.dll", "nvngx_dlssg.dll"])

    def test_an_older_sdk_folder_still_installs_dlss(self):
        # (an SDK folder fetched before frame generation's runtime was in the table: DLSS's goes in, fetch() adds the other)
        rt = ngx_sdk.runtime(self.sdk, "windows")
        os.makedirs(os.path.dirname(rt))
        Path(rt).write_bytes(b"runtime")
        game = os.path.join(self.temp.name, "game")
        dst = ngx_sdk.install_runtime(self.sdk, "windows", os.path.join(game, "ngx"))
        self.assertEqual(os.listdir(os.path.join(game, "ngx")), ["nvngx_dlss.dll"])
        self.assertEqual(Path(dst).read_bytes(), b"runtime")


class LicenseTests(unittest.TestCase):
    def test_notice_names_nvidia_and_the_license(self):
        self.assertIn("NVIDIA", ngx_sdk.NOTICE)
        self.assertIn(ngx_sdk.LICENSE_URL, ngx_sdk.NOTICE)
        self.assertIn("Never share", ngx_sdk.NOTICE)

    def test_flag_or_a_yes(self):
        self.assertTrue(ngx_sdk.accepted(True, ask=lambda _: self.fail("asked"), interactive=False))
        self.assertFalse(ngx_sdk.accepted(False, ask=lambda _: self.fail("asked"), interactive=False))
        self.assertTrue(ngx_sdk.accepted(False, ask=lambda _: "yes", interactive=True))
        self.assertFalse(ngx_sdk.accepted(False, ask=lambda _: "", interactive=True))
        self.assertFalse(ngx_sdk.accepted(False, ask=lambda _: "no", interactive=True))

    def test_nothing_downloaded_without_acceptance(self):
        with mock.patch.object(ngx_sdk, "fetch", side_effect=AssertionError("fetched")), \
                mock.patch.object(ngx_sdk, "accepted", return_value=False), \
                mock.patch("sys.stdout", io.StringIO()), mock.patch("sys.stderr", io.StringIO()) as err:
            self.assertEqual(ngx_sdk.main(["--platform", "windows", "x"]), 2)
        self.assertIn("not accepted", err.getvalue())


if __name__ == "__main__":
    unittest.main()
