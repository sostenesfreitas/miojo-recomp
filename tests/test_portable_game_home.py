"""Exercise the production game association without game files, a build or a child process."""
import ast
import contextlib
import ntpath
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_SKIP = {"linux": "Windows portable Builder association"}


@unittest.skipUnless(os.name == "nt", "Windows association contract")
class Association(unittest.TestCase):
    def test_relocation_and_failures(self):
        source = ast.parse((ROOT / "nx2/make_game.py").read_text(encoding="utf-8"))
        nodes = [n for n in source.body if isinstance(n, ast.FunctionDef) and
                 n.name in ("portable_game_home", "write_game_home")]
        self.assertEqual(len(nodes), 2)
        with tempfile.TemporaryDirectory(prefix="portable-game-") as folder:
            root = Path(folder)
            tree = root / "authored ü, tree"
            home = tree / "Builder"
            game = tree / "Game"
            tools = home / "versions/v1/HookshotBuilder/tools"
            tools.mkdir(parents=True)
            game.mkdir()
            marker = home / ".hookshot-portable"
            marker.write_bytes(b"Hookshot Builder portable home v1\n")
            setup = tools.parent / "HookshotBuilder-Setup.exe"
            setup.write_text("authored fixture; never executable")
            namespace = dict(os=os, ntpath=ntpath, tempfile=tempfile, contextlib=contextlib, ROOT=str(tools))
            exec(compile(ast.Module(body=nodes, type_ignores=[]), "make_game.py", "exec"), namespace)
            resolve, write = namespace["portable_game_home"], namespace["write_game_home"]
            main_body = next(n for n in source.body if isinstance(n, ast.FunctionDef) and n.name == "main").body
            early = next(n for n in main_body if isinstance(n, ast.Assign) and
                         any(isinstance(t, ast.Name) and t.id == "portable_home" for t in n.targets))
            version_loop = next(n for n in main_body if isinstance(n, ast.For) and
                                isinstance(n.target, ast.Name) and n.target.id == "version_name")
            publish = next(n for n in main_body if isinstance(n, ast.If) and
                           isinstance(n.test, ast.Name) and n.test.id == "tag")
            with patch.dict(os.environ, {"HOOKSHOT_SETUP_BASE": str(home), "HOOKSHOT_BUILDER_PORTABLE": "1"}):
                relative = resolve(str(game))
                self.assertFalse(ntpath.isabs(relative))
                write(str(game), relative)
                pointer = game / ".hookshot_builder_home"
                self.assertEqual(pointer.read_bytes(), relative.encode("utf-8"))
                # Execute the actual early-main validation and final version-publication statements,
                # including the call to write_game_home, without replacing that control flow.
                (tools / "version.txt").write_text("v-authored")
                pointer.unlink()
                namespace.update(a=SimpleNamespace(check=False, update=None, out=str(game)), out=str(game), tag="")
                exec(compile(ast.Module(body=[early, version_loop, publish], type_ignores=[]), "make_game.py", "exec"), namespace)
                self.assertEqual((game / "hookshot_version.txt").read_text(), "version=v-authored\nbuilder_local=1\n")
                self.assertEqual(pointer.read_bytes(), relative.encode("utf-8"))
                original = pointer.read_bytes()
                for bad in (b"wrong", b"\xef\xbb\xbfHookshot Builder portable home v1", b"x" * 1000):
                    marker.write_bytes(bad)
                    with self.assertRaises(ValueError):
                        resolve(str(game))
                    self.assertEqual(pointer.read_bytes(), original)
                marker.write_bytes(b"Hookshot Builder portable home v1")
                setup.unlink()
                with self.assertRaises(ValueError):
                    resolve(str(game))
                self.assertEqual(pointer.read_bytes(), original)
                setup.write_text("authored")
                moved = root / "moved tree"
                tree.rename(moved)
                self.assertEqual((moved / "Game" / relative).resolve(), (moved / "Builder").resolve())
                namespace["ROOT"] = str(moved / tools.relative_to(tree))
                os.environ["HOOKSHOT_SETUP_BASE"] = str(moved / "Builder")
                self.assertEqual(resolve(str(moved / "Game")), relative)
                os.environ["HOOKSHOT_BUILDER_PORTABLE"] = "0"
                self.assertIsNone(resolve(str(moved / "Game")))
                # A successful standard rebuild must replace the portable association,
                # not leave setup and the game's updater choosing different Builders.
                standard = str(root / "standard/HookshotBuilder.exe")
                namespace.update(out=str(moved / "Game"),
                                 a=SimpleNamespace(check=False, update=None, out=str(moved / "Game")))
                statements = ast.Module(body=[early, version_loop, publish], type_ignores=[])
                with patch.dict(os.environ, {"HOOKSHOT_BUILDER_EXE": standard}):
                    exec(compile(statements, "make_game.py", "exec"), namespace)
                    self.assertEqual((moved / "Game/hookshot_version.txt").read_text(),
                                     "version=v-authored\nbuilder=%s\n" % standard)
                    self.assertFalse((moved / "Game/.hookshot_builder_home").exists())
                    # A second standard rebuild has no association to remove.
                    exec(compile(statements, "make_game.py", "exec"), namespace)
                # Failure to remove an association must not publish standard metadata
                # while the native setup would still follow the old portable pointer.
                pointer = moved / "Game/.hookshot_builder_home"
                pointer.write_bytes(original)
                version = moved / "Game/hookshot_version.txt"
                version.write_text("version=old\nbuilder_local=1\n")
                with patch.object(namespace["os"], "remove", side_effect=PermissionError("authored denial")):
                    with self.assertRaises(PermissionError):
                        exec(compile(statements, "make_game.py", "exec"), namespace)
                self.assertEqual(version.read_text(), "version=old\nbuilder_local=1\n")
                self.assertEqual(pointer.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
