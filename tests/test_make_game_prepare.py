"""Game-free checks of make_game's --game and --prepare (RecompLauncher): the copy chosen, and the progress lines.

Run with: python -I tests/test_make_game_prepare.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class PrepareTests(unittest.TestCase):
    def tearDown(self):
        make_game.PREPARE = False

    def parse(self, *argv):
        with mock.patch.object(sys, "argv", ["make_game.py", *argv]):
            return make_game.parse_args()

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.nup = os.path.join(self.dir.name, "game.nup")
        Path(self.nup).write_bytes(b"")

    def fails(self, *argv):
        """The lines a refused parse printed, and that it stopped with exit code 1."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit) as stop:
            self.parse(*argv)
        self.assertEqual(stop.exception.code, 1)
        return out.getvalue().splitlines()

    def test_game_file_is_the_nup(self):
        a = self.parse("--game", self.nup, "--out", "o")
        self.assertEqual((a.nup, a.romfs, a.exefs, a.prepare), (self.nup, None, None, False))
        self.assertFalse(make_game.PREPARE)

    def test_game_folder_is_romfs_and_exefs(self):
        d = self.dir.name
        os.mkdir(os.path.join(d, "romfs"))
        os.mkdir(os.path.join(d, "exefs"))
        a = self.parse("--game", d, "--out", "o")
        self.assertEqual((a.nup, a.romfs, a.exefs), (None, os.path.join(d, "romfs"), os.path.join(d, "exefs")))

    def test_prepare_defaults_the_out_folder(self):
        a = self.parse("--game", self.nup, "--prepare")
        self.assertTrue(make_game.PREPARE)
        self.assertEqual(a.out, os.path.join(os.path.expanduser("~"), "Games", "Hookshot"))
        self.assertEqual(self.parse("--game", self.nup, "--prepare", "--out", "o").out, "o")

    def test_missing_game_is_an_error_line(self):
        missing = os.path.join(self.dir.name, "nothing.nup")
        self.assertEqual(self.fails("--game", missing, "--prepare"), ["error Your copy of the game was not found: " + missing])

    def test_folder_without_romfs_and_exefs_is_an_error_line(self):
        os.mkdir(os.path.join(self.dir.name, "exefs"))
        lines = self.fails("--game", self.dir.name, "--prepare", "--out", os.path.join(self.dir.name, "out"))
        self.assertEqual(lines, ["error The folder %s needs both a romfs and an exefs folder in it." % self.dir.name])
        self.assertFalse(os.path.exists(os.path.join(self.dir.name, "out")))

    def test_without_prepare_a_missing_game_is_the_app_line(self):
        self.assertEqual(self.fails("--game", os.path.join(self.dir.name, "x.nup"))[0][:8], "@@error ")

    def test_error_and_check_lines_are_on_stdout(self):
        make_game.PREPARE = True
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            make_game.say("error", "The compile stopped.")
            make_game.say("check", "ok", "This is the version Hookshot supports (build e0f3ccfa).")
        self.assertEqual(out.getvalue().splitlines(),
                         ["error The compile stopped.", "check ok This is the version Hookshot supports (build e0f3ccfa)."])
        self.assertEqual(err.getvalue(), "")

    def test_progress_lines(self):
        make_game.PREPARE = True
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            make_game.say("stage", "compile", "Compiling")
            make_game.say("progress", "0.5")
            make_game.say("times", "1", "1", "-1")
            make_game.say("done", "o")
        self.assertEqual(out.getvalue().splitlines(), ["progress 0.250 Compiling", "progress 0.600 Compiling", "progress 1.000 Done"])

    def test_without_prepare_the_app_lines_are_unchanged(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            make_game.say("stage", "read", "Reading your game files")
        self.assertEqual(out.getvalue(), "@@stage read Reading your game files\n")


class LanguageTests(unittest.TestCase):
    """What a build writes to language.txt when no --language says (a launcher's build has no window to choose one in)."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.addCleanup(setattr, make_game, "PREPARE", False)
        self.out = os.path.join(self.dir.name, "Hookshot")
        os.mkdir(self.out)
        self.nup = os.path.join(self.dir.name, "game.nup")
        Path(self.nup).write_bytes(b"")

    def args(self, *argv):
        with mock.patch.object(sys, "argv", ["make_game.py", *argv]):
            return make_game.parse_args()

    def folder_says(self, code):
        """language.txt as a build leaves it, with the code the player then put in it"""
        with open(os.path.join(self.out, "language.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\n\n%s\n\n" % code)
            f.writelines("# %-8s %s\n" % row for row in make_game.LANGUAGES)

    def language(self, *argv, system="en_US.UTF-8"):
        with mock.patch.object(make_game, "system_language", return_value=make_game.system_language(system)):
            return make_game.build_language(self.args("--out", self.out, *argv), self.out)

    def test_a_launcher_build_keeps_the_language_the_player_set(self):
        self.folder_says("de")
        self.assertEqual(self.language("--game", self.nup, "--prepare"), "de")
        self.assertEqual(self.language("--game", self.nup, "--prepare", system="fr_FR.UTF-8"), "de")

    def test_a_first_launcher_build_takes_the_systems_language(self):
        self.assertEqual(self.language("--game", self.nup, "--prepare", system="pt_BR.UTF-8"), "pt-BR")
        self.assertEqual(self.language("--game", self.nup, "--prepare", system="C"), "en-US")
        self.folder_says("not a code")
        self.assertEqual(self.language("--game", self.nup, "--prepare", system="ja_JP.UTF-8"), "ja")

    def test_the_language_asked_for_is_the_one_built(self):
        self.folder_says("de")
        self.assertEqual(self.language("--nup", "g.nup", "--language", "fr"), "fr")   # (the builder's window: its own choice)
        self.assertEqual(self.language("--game", self.nup, "--prepare", "--language", "it", system="ja_JP.UTF-8"), "it")

    def test_a_build_without_a_window_or_a_word_is_english_or_the_folders(self):
        self.assertEqual(self.language("--nup", "g.nup", system="ja_JP.UTF-8"), "en-US")
        self.folder_says("ko")
        self.assertEqual(self.language("--nup", "g.nup", system="ja_JP.UTF-8"), "ko")

    def test_an_update_keeps_the_folders_language(self):
        self.folder_says("es-419")
        a = self.args("--update", self.out, "--language", "fr")
        self.assertEqual(make_game.build_language(a, self.out), "es-419")
        os.remove(os.path.join(self.out, "language.txt"))
        self.assertEqual(make_game.build_language(a, self.out), "fr")

    def test_the_systems_language_is_the_closest_the_game_has(self):
        for name, code in (("de_DE.UTF-8", "de"), ("de-AT", "de"), ("en_GB.UTF-8", "en-GB"), ("en_AU.UTF-8", None), ("fr_CA", "fr-CA"),
                           ("fr_BE.UTF-8@euro", "fr"), ("es_ES.UTF-8", "es"), ("es", "es"), ("es_MX.UTF-8", "es-419"), ("es-419", "es-419"),
                           ("pt_PT.UTF-8", "pt-BR"), ("zh_CN.UTF-8", "zh-Hans"), ("zh_TW.UTF-8", "zh-Hant"), ("zh-Hant-HK", "zh-Hant"),
                           ("zh_SG", "zh-Hans"), ("ja_JP.eucJP", "ja"), ("sv_SE.UTF-8", None), ("C", None), ("POSIX", None), ("", None)):
            self.assertEqual(make_game.system_language(name), code, name)

    @unittest.skipIf(os.name == "nt", "the environment's names: Linux")
    def test_the_systems_language_is_read_as_gettext_reads_it(self):
        for env, code in (({"LANG": "nl_NL.UTF-8"}, "nl"), ({"LANG": "nl_NL.UTF-8", "LC_MESSAGES": "pl_PL.UTF-8"}, "pl"),
                          ({"LANG": "nl_NL.UTF-8", "LC_MESSAGES": "pl_PL.UTF-8", "LC_ALL": "ko_KR.UTF-8"}, "ko"),
                          ({"LANG": "ru_RU.UTF-8", "LC_ALL": ""}, "ru"), ({}, None),
                          # (LANGUAGE's first entry over them, as GNOME's language setting leaves it; not under the C locale)
                          ({"LANG": "en_US.UTF-8", "LANGUAGE": "de:en"}, "de"), ({"LC_ALL": "fr_FR.UTF-8", "LANGUAGE": "it"}, "it"),
                          ({"LANG": "C.UTF-8", "LANGUAGE": "de:en"}, None), ({"LANGUAGE": "de"}, None),
                          ({"LANG": "nl_NL.UTF-8", "LANGUAGE": ""}, "nl")):
            clean = {k: v for k, v in os.environ.items() if k not in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE")}
            with mock.patch.dict(os.environ, dict(clean, **env), clear=True):
                self.assertEqual(make_game.system_language(), code, env)

    def test_a_launcher_build_over_a_folder_goes_on_in_its_language(self):
        """main itself, up to its first step: the language it builds in is the one the player left in the folder"""
        class Stop(Exception):
            pass
        args = []
        real = make_game.parse_args
        self.folder_says("ru")
        argv = ["make_game.py", "--game", self.nup, "--prepare", "--out", self.out, "--work", os.path.join(self.dir.name, "work")]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(make_game, "warn_slow_disk", side_effect=Stop), \
             mock.patch.object(make_game, "parse_args", side_effect=lambda: args.append(real()) or args[0]), \
             contextlib.redirect_stdout(io.StringIO()), self.assertRaises(Stop):
            make_game.main()
        self.assertEqual(args[0].language, "ru")


if __name__ == "__main__":
    unittest.main()
