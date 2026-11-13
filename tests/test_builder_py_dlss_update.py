"""The Python Windows builder's update keeps a folder's NVIDIA DLSS by the license accepted (builder/py/hookshot_builder/window.py,
update_build): the settings' DlssLicense (the WPF builder's builder-settings.json, which both builders share), or for a license
accepted before the record, the SDK's LICENSE.txt. With the SDK's files alone as the record, an SDK deleted or a new builder
folder made the update build without DLSS, and its runtime went.

update_build is taken from the source as it is and run with stand-ins for the window and core (no Qt, no build): the DLSS
decision it hands to run() for each case. Authored placeholders only: no NVIDIA SDK, runtime or game data.

python -I -B tests/test_builder_py_dlss_update.py
"""
import ast
import os
from pathlib import Path
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]


def update_build():
    """update_build, from window.py's Builder class, as a function of (self, tools, folder) over a namespace of stand-ins"""
    tree = ast.parse((ROOT / "builder/py/hookshot_builder/window.py").read_text(encoding="utf-8"))
    method = next(n for c in tree.body if isinstance(c, ast.ClassDef) for n in c.body
                  if isinstance(n, ast.FunctionDef) and n.name == "update_build")
    return method


class PyBuilderDlssUpdate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="py-builder-dlss-")
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.sdk, self.game, self.settings = root / "ngx-sdk", root / "game", {}
        self.game.mkdir()
        core = types.SimpleNamespace(load_settings=lambda: dict(self.settings), ngx_sdk=lambda: str(self.sdk))
        self.namespace = dict(os=os, core=core, SHADERS_LATER="--no-prepare-shaders")
        exec(compile(ast.Module(body=[update_build()], type_ignores=[]), "window.update_build", "exec"), self.namespace)

    def decision(self):
        """the dlss= update_build hands to run()"""
        got = {}
        win = types.SimpleNamespace(download=None, rows={}, prepare_now=True,
                                    run=lambda tools, args, keep_rows, dlss: got.update(dlss=dlss, args=args))
        self.namespace["update_build"](win, "tools", str(self.game))
        self.assertEqual(got["args"], ["--update", str(self.game)])
        return got["dlss"]

    def runtime(self):
        (self.game / "ngx").mkdir(exist_ok=True)
        (self.game / "ngx" / "nvngx_dlss.dll").write_text("authored runtime placeholder")

    def license_file(self):
        self.sdk.mkdir(exist_ok=True)
        (self.sdk / "LICENSE.txt").write_text("authored license placeholder")

    def test_no_runtime_no_dlss(self):
        self.settings["DlssLicense"] = True
        self.license_file()
        self.assertFalse(self.decision())   # (the license alone did not build this game's DLSS)

    def test_the_settings_record_keeps_dlss_without_the_sdk(self):
        self.runtime()
        self.assertFalse(self.decision())   # (a leftover DLL alone does not accept NVIDIA's license)
        self.settings["DlssLicense"] = True
        self.assertTrue(self.decision())    # (the SDK's files gone: the license accepted is in the settings)

    def test_a_license_accepted_before_the_record(self):
        self.runtime()
        self.license_file()
        self.assertTrue(self.decision())    # (no record yet: the SDK's LICENSE.txt, as before)


if __name__ == "__main__":
    unittest.main()
