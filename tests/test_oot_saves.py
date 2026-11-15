"""scripts/oot_saves.py, game-free: finding every emulator's folders, importing and exporting, the backups, the guards.
Temp folders only: HOME, XDG_DATA_HOME, APPDATA and LOCALAPPDATA all point into one.

python -I -B tests/test_oot_saves.py
"""
import contextlib
import importlib.util
import io
import json
import os
import shutil
from pathlib import Path
import tempfile
import unittest
from unittest import mock

# (Linux only)
RUN_ALL_SKIP = {
    "windows": "the save script's folder discovery is the Linux one"
               " (on Windows Hookshot's saves are in the game folder: --hookshot)",
}

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("oot_saves", ROOT / "scripts" / "oot_saves.py")
oot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oot)

UID = "0123456789abcdef0123456789abcdef"
PROFILE = "FEDCBA9876543210FEDCBA9876543210"
ACCOUNT = "00112233445566778899aabbccddeeff"


def make_save(folder, tag, slots=("slot_00", "slot_01")):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "account.sav").write_bytes(b"account-" + tag)
    (folder / "option.sav").write_bytes(b"option-" + tag)
    for s in slots:
        (folder / s).mkdir()
        (folder / s / "progress.sav").write_bytes(b"progress-" + tag + s.encode())
        (folder / s / "caption.sav").write_bytes(b"caption-" + tag + s.encode())


def tree(folder):
    return {str(p.relative_to(folder)): p.read_bytes() for p in sorted(folder.rglob("*")) if p.is_file()}


def eden_save(data, profile=PROFILE):
    return data / "nand" / "user" / "save" / oot.NO_ACCOUNT / profile / oot.TITLE


class Case(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.home = self.tmp / "home"
        self.data = self.home / ".local" / "share"
        self.appdata = self.home / "AppData" / "Roaming"
        self.localappdata = self.home / "AppData" / "Local"
        self.hs = self.data / "hookshot" / "save" / UID
        self.li = eden_save(self.data / "eden")
        make_save(self.hs, b"HS")
        make_save(self.li, b"LI", slots=("slot_00",))
        self.backups = self.tmp / "backups"
        env = mock.patch.dict(os.environ, {"HOME": str(self.home), "USERPROFILE": str(self.home), "XDG_DATA_HOME": str(self.data),
                                           "APPDATA": str(self.appdata), "LOCALAPPDATA": str(self.localappdata)})
        env.start()
        self.addCleanup(env.stop)
        idle = mock.patch.object(oot, "running_games", return_value=[])
        self.running = idle.start()
        self.addCleanup(idle.stop)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = oot.main(list(args) + ["--backup-dir", str(self.backups)])
        return code, out.getvalue(), err.getvalue()

    def backup_dirs(self):
        return sorted(self.backups.iterdir()) if self.backups.exists() else []


class Discovery(Case):
    def test_every_entry_on_linux(self):
        make_save(eden_save(self.data / "eglantine"), b"EG")
        nx = self.home / "Games" / "Ocarina" / "profile" / "save" / ACCOUNT
        make_save(nx, b"NX")
        ls = self.data / "longshot" / "save" / UID
        make_save(ls, b"LS")
        expect = {"lichii": self.li, "frizti": self.li, "orchid": self.li, "pokedinner": self.li, "eden": self.li,
                  "eglantine": eden_save(self.data / "eglantine"), "nx2navi": nx, "longshot": ls}
        self.assertEqual(sorted(expect), sorted(p.name for p in oot.EMULATORS))
        for program in oot.EMULATORS:
            folder, found = oot.locate(program, None, "--from")
            self.assertEqual(list(found.values()), [str(expect[program.name])], program.name)
        self.assertEqual(oot.locate(oot.HOOKSHOT, None, "--hookshot")[1], {UID: str(self.hs)})

    def test_every_entry_on_windows(self):
        win_li = eden_save(self.appdata / "eden")
        win_eg = eden_save(self.appdata / "eglantine")
        win_nx = self.localappdata / "Programs" / "NX2navi" / "game" / "profile" / "save" / ACCOUNT
        win_ls = self.home / "Games" / "Longshot" / "user" / "save" / UID
        for folder in (win_li, win_eg, win_nx, win_ls):
            make_save(folder, b"W")
        expect = {"lichii": win_li, "frizti": win_li, "orchid": win_li, "pokedinner": win_li, "eden": win_li,
                  "eglantine": win_eg, "nx2navi": win_nx, "longshot": win_ls}
        with mock.patch.object(oot, "WINDOWS", True):
            for program in oot.EMULATORS:
                self.assertEqual(list(oot.locate(program, None, "--from")[1].values()), [str(expect[program.name])])
            with self.assertRaises(oot.Fail) as e:
                oot.locate(oot.HOOKSHOT, None, "--hookshot")
            self.assertIn("--hookshot", str(e.exception))
            game = self.tmp / "Hookshot"
            make_save(game / "user" / "save" / UID, b"W")
            self.assertEqual(oot.locate(oot.HOOKSHOT, str(game), "--hookshot")[1], {UID: str(game / "user" / "save" / UID)})

    def test_lichii_s_own_folder_first(self):
        # (Lichii from its build 150 on keeps %APPDATA%\\lichii, ~/.local/share/lichii; the others stay in Eden's)
        own = eden_save(self.data / "lichii")
        make_save(own, b"L2", slots=("slot_02",))
        by_name = {p.name: p for p in oot.EMULATORS}
        self.assertEqual(oot.locate(by_name["lichii"], None, "--from"), (str(self.data / "lichii"), {PROFILE: str(own)}))
        for name in ("frizti", "orchid", "pokedinner", "eden"):
            self.assertEqual(list(oot.locate(by_name[name], None, "--from")[1].values()), [str(self.li)], name)
        win_own, win_eden = eden_save(self.appdata / "lichii"), eden_save(self.appdata / "eden")
        make_save(win_own, b"W2")
        make_save(win_eden, b"W1")
        with mock.patch.object(oot, "WINDOWS", True):
            self.assertEqual(list(oot.locate(by_name["lichii"], None, "--from")[1].values()), [str(win_own)])
            self.assertEqual(list(oot.locate(by_name["orchid"], None, "--from")[1].values()), [str(win_eden)])

    def test_lichii_s_own_folder_without_a_save_leaves_the_older_one(self):
        (self.data / "lichii" / "nand" / "user" / "save").mkdir(parents=True)   # (started, the game never saved there)
        by_name = {p.name: p for p in oot.EMULATORS}
        self.assertEqual(list(oot.locate(by_name["lichii"], None, "--from")[1].values()), [str(self.li)])

    def test_lichii_s_copy_goes_to_its_own_folder(self):
        own = eden_save(self.data / "lichii")
        make_save(own, b"L2", slots=("slot_02",))
        before = tree(self.li)
        code, out, err = self.run_main("export", "lichii")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(own), tree(self.hs))
        self.assertEqual(tree(self.li), before, "Eden's folder was written for Lichii")
        (own / "account.sav").write_bytes(b"played on in Lichii")
        code, out, err = self.run_main("import", "lichii")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.hs), tree(own))
        self.assertEqual(tree(self.li), before)

    def test_aliases_of_names(self):
        self.assertIs(oot.program_named("Fritchii"), oot.program_named("frizti"))
        with self.assertRaises(oot.Fail):
            oot.program_named("ryujinx")

    def test_deeper_and_shallower_folders(self):
        data = self.data / "eden"
        for root in (data, data / "nand", data / "nand" / "user" / "save", self.li.parent.parent, self.li.parent,
                     self.li):
            self.assertEqual(oot.find_saves(str(root)), {PROFILE: str(self.li)}, root)
        for root in (self.data / "hookshot", self.hs.parent, self.hs):
            self.assertEqual(oot.find_saves(str(root)), {UID: str(self.hs)}, root)

    def test_empty_title_folder_is_a_target(self):
        empty = eden_save(self.tmp / "portable" / "user", "11111111111111111111111111111111")
        empty.mkdir(parents=True)
        self.assertEqual(oot.find_saves(str(self.tmp / "portable" / "user")),
                         {"11111111111111111111111111111111": str(empty)})
        self.assertEqual(oot.find_saves(str(empty)), {"11111111111111111111111111111111": str(empty)})

    def test_eden_newer_layout(self):
        data = self.tmp / "newer"
        uuid = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
        save = data / "nand" / "user" / "save" / "account" / uuid / oot.TITLE / "0"
        make_save(save, b"NEW")
        for root in (data, save.parent, save):
            self.assertEqual(oot.find_saves(str(root)), {uuid: str(save)}, root)

    def test_unrelated_folders_are_not_saves(self):
        (self.tmp / "junk" / "a" / "0").mkdir(parents=True)
        (self.tmp / "junk" / "b" / "save" / UID).mkdir(parents=True)
        self.assertEqual(oot.find_saves(str(self.tmp / "junk")), {})

    def test_missing_folders(self):
        with self.assertRaises(oot.Fail):
            oot.locate(oot.FOLDER, str(self.tmp / "nothing"), "--from")
        (self.tmp / "empty").mkdir()
        with self.assertRaises(oot.Fail):
            oot.locate(oot.FOLDER, str(self.tmp / "empty"), "--from")
        with self.assertRaises(oot.Fail) as e:
            oot.locate(oot.program_named("eglantine"), None, "--from")
        self.assertIn(str(self.data / "eglantine"), str(e.exception))

    def test_list_json(self):
        make_save(self.data / "longshot" / "save" / UID, b"LS")
        code, out, _ = self.run_main("list", "--json")
        self.assertEqual(code, 0)
        doc = json.loads(out)
        self.assertEqual([s["path"] for s in doc["hookshot"]["saves"]], [str(self.hs)])
        by = {g["name"]: g for g in doc["sources"]}
        self.assertEqual(sorted(by), ["lichii", "longshot"])   # (Eden's forks share one entry; no other saves here)
        self.assertIn("Orchid", by["lichii"]["display"])
        self.assertEqual([x["slot"] for x in by["lichii"]["saves"][0]["slots"]], ["slot_00"])
        self.assertEqual(by["longshot"]["saves"][0]["profile"], UID)

    def test_import_json_answer(self):
        code, out, err = self.run_main("import", "lichii", "--json")
        self.assertEqual(code, 0, err)
        doc = json.loads(out)
        self.assertTrue(doc["ok"])
        self.assertEqual((doc["from"], doc["to"], doc["slots"]), (str(self.li), str(self.hs), ["slot_00"]))
        self.assertEqual(tree(self.hs), tree(self.li))
        code, out, _ = self.run_main("import", "eglantine", "--json")
        self.assertEqual(code, 1)
        self.assertIn("no Eglantine save", json.loads(out)["error"])

    def test_import_into_a_hookshot_without_a_save(self):
        shutil.rmtree(self.data / "hookshot")
        code, out, err = self.run_main("import", "lichii")
        self.assertEqual(code, 0, err)
        made = self.data / "hookshot" / "save" / oot.USER_ID
        self.assertEqual(tree(made), tree(self.li))
        with mock.patch.object(oot, "WINDOWS", True):
            game = self.tmp / "Games" / "Hookshot"
            code, out, err = self.run_main("import", "--from", str(self.li), "--hookshot", str(game))
            self.assertEqual(code, 1)
            self.assertIn("build the game there first", err)
            game.mkdir(parents=True)
            code, out, err = self.run_main("import", "--from", str(self.li), "--hookshot", str(game), "--json")
            self.assertEqual(code, 0, err)
            self.assertEqual(json.loads(out)["to"], str(game / "user" / "save" / oot.USER_ID))
            self.assertEqual(tree(game / "user" / "save" / oot.USER_ID), tree(self.li))

    def test_ambiguous_profiles_listed(self):
        other = "11111111111111111111111111111111"
        make_save(eden_save(self.data / "eden", other), b"O")
        before = tree(self.li)
        code, out, err = self.run_main("export", "lichii")
        self.assertEqual(code, 1)
        self.assertIn(PROFILE, err)
        self.assertIn(other, err)
        self.assertIn("--profile", err)
        self.assertEqual(tree(self.li), before)
        self.assertEqual(self.backup_dirs(), [])
        code, out, err = self.run_main("export", "lichii", "--profile", other.lower())
        self.assertEqual(code, 0, err)

    def test_list(self):
        make_save(self.home / "Games" / "Ocarina" / "profile" / "save" / ACCOUNT, b"NX")
        code, out, err = self.run_main("list")
        self.assertEqual(code, 0, err)
        for text in (UID, PROFILE, ACCOUNT, "slot_01", "Lichii / Frizti (Fritchii) / Orchid / PokeDinner / Eden", "NX2navi",
                     "Eglantine: nothing in"):
            self.assertIn(text, out)

    def test_list_with_lichii_s_own_folder(self):
        make_save(eden_save(self.data / "lichii"), b"L2", slots=("slot_02",))
        code, out, err = self.run_main("list")
        self.assertEqual(code, 0, err)
        self.assertIn("Lichii: %s\n" % (self.data / "lichii"), out)
        self.assertIn("Frizti (Fritchii) / Orchid / PokeDinner / Eden (one shared folder): %s\n" % (self.data / "eden"), out)
        self.assertIn("slot_02", out)
        self.assertEqual(out.count(str(self.data / "eden") + "\n"), 1)

    def test_list_with_no_emulator_save(self):
        for f in sorted(self.li.rglob("*"), reverse=True):
            f.unlink() if f.is_file() else f.rmdir()
        self.li.rmdir()
        code, out, err = self.run_main("list")
        self.assertEqual(code, 0, err)
        self.assertIn("Lichii: nothing in %s, %s\n" % (self.data / "lichii", self.data / "eden"), out)
        self.assertIn("Frizti (Fritchii) / Orchid / PokeDinner / Eden (one shared folder): nothing in %s\n" % (self.data / "eden"), out)

    def test_list_without_hookshot(self):
        (self.hs / "account.sav").unlink()
        for s in ("slot_00", "slot_01"):
            for f in (self.hs / s).iterdir():
                f.unlink()
            (self.hs / s).rmdir()
        code, out, err = self.run_main("list")
        self.assertEqual(code, 0, err)
        self.assertIn("Hookshot: no Hookshot save found", out)
        self.assertIn(PROFILE, out)


class Copy(Case):
    def test_export(self):
        before = tree(self.li)
        code, out, err = self.run_main("export", "lichii")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.li), tree(self.hs))
        (backup,) = self.backup_dirs()
        self.assertTrue(backup.name.startswith("export-lichii-"))
        self.assertEqual(tree(backup), before)

    def test_import(self):
        before = tree(self.hs)
        code, out, err = self.run_main("import", "orchid")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.hs), tree(self.li))
        self.assertNotIn("slot_01", tree(self.hs))
        (backup,) = self.backup_dirs()
        self.assertTrue(backup.name.startswith("import-orchid-"))
        self.assertEqual(tree(backup), before)

    def test_old_names_still_work(self):
        code, out, err = self.run_main("to-lichii")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.li), tree(self.hs))
        make_save(self.li / "slot_02", b"X", slots=())
        code, out, err = self.run_main("to-hookshot", "--lichii", str(self.data / "eden"))
        self.assertEqual(code, 0, err)
        self.assertIn("slot_02", {Path(k).parts[0] for k in tree(self.hs)})
        names = [b.name for b in self.backup_dirs()]
        self.assertTrue(names[0].startswith("export-lichii-") and names[1].startswith("import-lichii-"), names)

    def test_from_a_save_folder_and_a_data_folder(self):
        other = self.tmp / "backup of a save"
        make_save(other, b"F", slots=("slot_03",))
        code, out, err = self.run_main("import", "--from", str(other))
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.hs), tree(other))
        portable = self.tmp / "Lichii" / "user"
        make_save(eden_save(portable), b"P", slots=("slot_04",))
        code, out, err = self.run_main("import", "--from", str(portable))
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.hs), tree(eden_save(portable)))
        self.assertTrue(all(b.name.startswith("import-folder-") for b in self.backup_dirs()))

    def test_to_a_folder(self):
        portable = self.tmp / "Eglantine" / "user"
        target = eden_save(portable)
        target.mkdir(parents=True)   # (the game started once, nothing saved yet)
        code, out, err = self.run_main("export", "eglantine", "--to", str(portable))
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(target), tree(self.hs))

    def test_from_and_to_belong_to_their_command(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                oot.main(["export", "--from", str(self.li)])
            with self.assertRaises(SystemExit):
                oot.main(["import", "--to", str(self.li)])
        code, out, err = self.run_main("import")
        self.assertEqual(code, 1)
        self.assertIn("--from", err)

    def test_links_in_a_save_refused(self):
        secret = self.tmp / "secret.txt"
        secret.write_bytes(b"not a save")
        before = tree(self.hs)
        for make in (lambda o: (o / "slot_00" / "progress.sav").unlink() or (o / "slot_00" / "progress.sav").symlink_to(secret),
                     lambda o: (o / "account.sav").unlink() or (o / "account.sav").symlink_to(secret),
                     lambda o: (o / "slot_00" / "deep").symlink_to(self.tmp, target_is_directory=True)):
            other = self.tmp / ("linked-%d" % len(list(self.tmp.glob("linked-*"))))
            make_save(other, b"L", slots=("slot_00",))
            make(other)
            code, out, err = self.run_main("import", "--from", str(other))
            self.assertEqual(code, 1, out)
            self.assertIn("is a link", err)
            self.assertEqual(tree(self.hs), before)
        self.assertEqual(secret.read_bytes(), b"not a save")

    def test_junction_refused(self):
        # (a Windows junction is a reparse point, not a symbolic link; os.path.isjunction is only in Python 3.12 and newer)
        other = self.tmp / "junction"
        make_save(other, b"J", slots=("slot_00",))
        deep = other / "slot_00" / "deep"
        deep.mkdir()
        real_lstat = os.lstat

        class Junction:
            def __init__(self, st):
                self.st_mode, self.st_file_attributes = st.st_mode, 0x400 | 0x10

        def lstat(path, *a, **k):
            st = real_lstat(path, *a, **k)
            return Junction(st) if os.path.abspath(path) == str(deep) else st
        before = tree(self.hs)
        with mock.patch.object(oot.os, "lstat", lstat):
            code, out, err = self.run_main("import", "--from", str(other))
        self.assertEqual(code, 1, out)
        self.assertIn("is a link", err)
        self.assertEqual(tree(self.hs), before)

    def test_same_folder_refused(self):
        code, out, err = self.run_main("import", "--from", str(self.hs))
        self.assertEqual(code, 1)
        self.assertIn("both the source and the target", err)

    def test_backup_inside_target_refused_before_copy(self):
        self.backups = self.hs / "backups"
        before = tree(self.hs)
        for extra in ((), ("--dry-run",)):
            with self.subTest(extra=extra), mock.patch.object(
                oot.shutil, "copytree", side_effect=AssertionError("recursive backup copy started")
            ):
                code, _, err = self.run_main("import", "--from", str(self.li), "--hookshot", str(self.hs), *extra)
                self.assertEqual(code, 1)
                self.assertIn("--backup-dir outside the target save folder", err)
                self.assertFalse(self.backups.exists())
                self.assertEqual(tree(self.hs), before)

    def test_keep_settings(self):
        code, out, err = self.run_main("export", "lichii", "--keep-settings")
        self.assertEqual(code, 0, err)
        self.assertEqual((self.li / "option.sav").read_bytes(), b"option-LI")
        self.assertEqual((self.li / "account.sav").read_bytes(), b"account-HS")
        self.assertEqual((self.li / "slot_01" / "progress.sav").read_bytes(), b"progress-HSslot_01")

    def test_dry_run_changes_nothing(self):
        before = (tree(self.hs), tree(self.li))
        self.running.return_value = ["Lichii"]   # (a dry run does not mind)
        code, out, err = self.run_main("export", "lichii", "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertEqual((tree(self.hs), tree(self.li)), before)
        self.assertEqual(self.backup_dirs(), [])
        self.assertIn("Would copy", out)

    def test_source_without_slots(self):
        for s in ("slot_00", "slot_01"):
            for f in (self.hs / s).iterdir():
                f.unlink()
            (self.hs / s).rmdir()
        before = tree(self.li)
        code, out, err = self.run_main("export", "lichii")
        self.assertEqual(code, 1)
        self.assertEqual(tree(self.li), before)


class NX2navi(Case):
    def setUp(self):
        super().setUp()
        self.game = self.home / "Games" / "Ocarina"
        self.nx = self.game / "profile" / "save" / ACCOUNT
        make_save(self.nx, b"NX", slots=("slot_05",))
        (self.game / "profile" / "account.uid").write_bytes(bytes(16))
        (self.game / "profile" / "save-tx").mkdir()

    def test_both_ways(self):
        code, out, err = self.run_main("import", "nx2navi")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.hs), tree(self.nx))
        make_save(self.hs / "slot_00", b"Z", slots=())
        code, out, err = self.run_main("export", "nx2navi", "--to", str(self.game))
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.nx), tree(self.hs))
        self.assertEqual((self.game / "profile" / "account.uid").read_bytes(), bytes(16))

    def test_waiting_save_refused(self):
        (self.game / "profile" / "save-tx" / "commit").write_bytes(b"pending")
        before = (tree(self.hs), tree(self.nx))
        for command in ("import", "export"):
            code, out, err = self.run_main(command, "nx2navi")
            self.assertEqual(code, 1)
            self.assertIn("save-tx", err)
        self.assertEqual((tree(self.hs), tree(self.nx)), before)
        self.assertEqual(self.backup_dirs(), [])
        code, out, err = self.run_main("import", "nx2navi", "--force")
        self.assertEqual(code, 0, err)


class Guard(Case):
    def test_refuses_while_running(self):
        self.running.return_value = ["Hookshot"]
        before = (tree(self.hs), tree(self.li))
        code, out, err = self.run_main("export", "lichii")
        self.assertEqual(code, 1)
        self.assertIn("Hookshot", err)
        self.assertEqual((tree(self.hs), tree(self.li)), before)
        self.assertEqual(self.backup_dirs(), [])

    def test_force(self):
        self.running.return_value = ["Lichii"]
        code, out, err = self.run_main("export", "lichii", "--force")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.li), tree(self.hs))

    def real(self):
        # (the real running_games, not the test's mock of it)
        real = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(real)
        return real

    def test_every_process_name_on_windows(self):
        real = self.real()
        names = real.process_names()
        for program in [real.HOOKSHOT] + real.EMULATORS:
            self.assertTrue(program.processes, program.name)
            for p in program.processes:
                self.assertIn(program.display, names[p])
        out = "".join('"%s.exe","%d","Console","1","10 K"\r\n' % (p.upper() if i % 2 else p, i)
                      for i, p in enumerate(names))
        out += '"notepad.exe","4","x","1","1 K"\r\n'
        with mock.patch.object(real, "WINDOWS", True), \
             mock.patch.object(real.subprocess, "run", return_value=mock.Mock(stdout=out)):
            self.assertEqual(real.running_games(), sorted(set(names.values())))

    def test_linux_processes_and_appimages(self):
        real = self.real()
        procs = {"10": ("orchid", "/opt/orchid/orchid"), "11": ("AppRun", "/home/x/eglantine-linux-x86_64.AppImage"),
                 "12": ("ocarina", "./ocarina"), "13": ("bash", "/bin/bash")}

        def fake_open(path, *a, **k):
            pid, what = path.split("/")[2:4]
            comm, argv0 = procs[pid]
            return io.BytesIO(argv0.encode() + b"\0-g\0") if what == "cmdline" else io.StringIO(comm + "\n")

        with mock.patch.object(real, "WINDOWS", False), \
             mock.patch.object(real.os, "listdir", return_value=list(procs) + ["self"]), \
             mock.patch("builtins.open", fake_open):
            self.assertEqual(real.running_games(), ["NX2navi", "Orchid", "PokeDinner or Eglantine"])


# ---- Saves in the repository: scrub, check-repo-saves and the JPEG they write and read (made-up files only) ----

def picture_jpeg(width, height):
    """A made-up JPEG with detail in it (an AC coefficient in every luma block), the frame of grey_jpeg's: what a real
    thumbnail looks like to the checks"""
    grey = oot.grey_jpeg(width, height)
    head = grey[:grey.index(b"\xff\xda") + 14]
    dc = [oot.huffman_codes(*oot.DC_LUMA), oot.huffman_codes(*oot.DC_CHROMA)]
    ac = [oot.huffman_codes(*oot.AC_LUMA), oot.huffman_codes(*oot.AC_CHROMA)]
    bits = []

    def put(code, length):
        bits.extend((code >> (length - 1 - i)) & 1 for i in range(length))

    for _ in range(-(-height // 16) * -(-width // 16)):
        for _ in range(4):
            put(*dc[0][0])
            put(*ac[0][0x01])
            put(1, 1)
            put(*ac[0][0x00])
        for _ in range(2):
            put(*dc[1][0])
            put(*ac[1][0x00])
    bits.extend([1] * (-len(bits) % 8))
    scan = bytearray()
    for i in range(0, len(bits), 8):
        byte = int("".join(map(str, bits[i:i + 8])), 2)
        scan += bytes([byte, 0]) if byte == 0xFF else bytes([byte])
    return head + bytes(scan) + b"\xff\xd9"


# (where the date's entries were in a real caption.sav, and a made-up date: 2026-11-14 09:41:07)
DATE_AT = {"month": 280, "second": 288, "minute": 304, "day": 312, "hour": 328, "year": 360}
DATE = {"year": 2026, "month": 11, "day": 14, "hour": 9, "minute": 41, "second": 7}
HEALTH_AT = 272   # (0x1e55ecc5, the health it seems: kept)


def caption(jpeg, scene=b"TestScene", map_=b"TestScene/Room_1", date=None, date_at=None, more=()):
    """A made-up caption.sav: date ({field: value}, default DATE) at date_at ({field: entry offset}, default DATE_AT;
    a field left out has no entry), and more [(field, entry offset)], a second entry for a field"""
    head = bytearray(oot.CAPTION_JPEG)
    head[:4] = oot.CAPTION_MAGIC
    head[oot.CAPTION_CHECK:oot.CAPTION_CHECK + 4] = oot.CAPTION_JPEG_LEN.to_bytes(4, "little")
    head[HEALTH_AT:HEALTH_AT + 8] = (0x1e55ecc5).to_bytes(4, "little") + (12).to_bytes(4, "little")
    keys, date = dict(oot.CAPTION_DATE_KEYS), dict(DATE, **(date or {}))
    for field, at in list((DATE_AT if date_at is None else date_at).items()) + list(more):
        head[at:at + 8] = keys[field].to_bytes(4, "little") + date[field].to_bytes(4, "little")
    head[oot.CAPTION_SCENE:oot.CAPTION_SCENE + len(scene)] = scene
    head[oot.CAPTION_MAP:oot.CAPTION_MAP + len(map_)] = map_
    head[oot.CAPTION_JPEG_LEN:oot.CAPTION_JPEG] = len(jpeg).to_bytes(4, "little")
    return bytes(head) + jpeg


NAME_AT = 100
PROGRESS = bytes(range(256)) * 2   # (made up; "PLAYER" at NAME_AT stands for a name)
PROGRESS = PROGRESS[:NAME_AT] + b"PLAYER\0\0" + PROGRESS[NAME_AT + 8:]


class RepoSaves(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.src = self.tmp / "profile-save"
        (self.src / "slot_02").mkdir(parents=True)
        (self.src / "account.sav").write_bytes(b"account")
        (self.src / "option.sav").write_bytes(b"option")
        (self.src / "slot_02" / "progress.sav").write_bytes(PROGRESS)
        self.picture = picture_jpeg(543, 306)
        (self.src / "slot_02" / "caption.sav").write_bytes(caption(self.picture))
        self.saves = self.tmp / "assets" / "saves"
        self.saves.mkdir(parents=True)
        (self.saves / "README.md").write_text("# Saves in the repository\n")

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = oot.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def scrub(self, name="kokiri-start", *extra):
        return self.run_main("scrub", str(self.src), "--slot", "02", "--out", str(self.saves / name),
                             "--name-field", "progress.sav:%d:8" % NAME_AT, *extra)

    def passing(self):
        code, out, err = self.scrub()
        self.assertEqual(code, 0, err)
        (self.saves / "kokiri-start" / "README.md").write_text("Kokiri Forest, by Link's house, child\n\nMade fresh.\n")
        self.assertEqual(oot.repo_save_problems(str(self.saves)), [])

    # -- the JPEG --

    def test_grey_jpeg_is_a_baseline_jpeg_of_that_size(self):
        for width, height in ((543, 306), (1, 1), (16, 16), (17, 9), (640, 360)):
            with self.subTest(size=(width, height)):
                jpeg = oot.grey_jpeg(width, height)
                markers = [m for m, _, _ in oot.jpeg_segments(jpeg)]
                self.assertEqual(markers, [0xE0, 0xDB, 0xDB, 0xC0, 0xC4, 0xC4, 0xC4, 0xC4, 0xDA])
                self.assertEqual(jpeg[:2] + jpeg[-2:], b"\xff\xd8\xff\xd9")
                self.assertEqual(oot.jpeg_size(jpeg), (width, height))
                blocks = oot.jpeg_blocks(jpeg)
                mcus = -(-width // 16) * -(-height // 16)
                self.assertEqual({c: (len(d), set(d), a) for c, (d, a) in blocks.items()},
                                 {1: (4 * mcus, {0}, False), 2: (mcus, {0}, False), 3: (mcus, {0}, False)})
                self.assertIsNone(oot.jpeg_is_mid_grey(jpeg))
        self.assertLess(len(oot.grey_jpeg(543, 306)), 4096)

    def test_grey_levels_read_back(self):
        # (every level: the DC written is the DC read, through the byte stuffing; only 128 is mid-grey)
        for level in range(0, 256, 5):
            jpeg = oot.grey_jpeg(33, 17, level)
            luma = oot.jpeg_blocks(jpeg)[1]
            self.assertEqual(set(luma[0]), {round((level - 128) * 8 / oot.QUANT_LUMA[0])})
            self.assertFalse(luma[1])
            self.assertEqual(oot.jpeg_is_mid_grey(jpeg) is None, level == 128, level)
        try:   # (an independent decoder, when this PC has one: the pixels are the level)
            from PIL import Image
        except ImportError:
            return
        for level in (0, 128, 200, 255):
            img = Image.open(io.BytesIO(oot.grey_jpeg(543, 306, level)))
            img.load()
            self.assertEqual(img.size, (543, 306))
            self.assertTrue(all(abs(lo - level) <= 1 and abs(hi - level) <= 1 for lo, hi in img.getextrema()))

    def test_a_picture_is_not_grey(self):
        self.assertEqual(oot.jpeg_size(self.picture), (543, 306))
        self.assertIn("detail", oot.jpeg_is_mid_grey(self.picture))
        grey = oot.grey_jpeg(543, 306)
        self.assertIn("ends early", oot.jpeg_is_mid_grey(grey[:len(grey) - 500] + b"\xff\xd9"))
        self.assertIn("after the scan", oot.jpeg_is_mid_grey(grey[:-2] + b"\x00\x00\xff\xd9"))
        self.assertIn("not baseline", oot.jpeg_is_mid_grey(grey.replace(b"\xff\xc0", b"\xff\xc2", 1)))
        self.assertIn("not a JPEG", oot.jpeg_is_mid_grey(b"\x89PNG\r\n\x1a\n" + grey))

    # -- caption.sav --

    def test_caption_layout(self):
        good = caption(self.picture)
        got = oot.caption_layout(good)
        self.assertEqual((got["scene"], got["map"], got["width"], got["height"], got["jpeg"]),
                         ("TestScene", "TestScene/Room_1", 543, 306, self.picture))
        spoilt = {
            "magic": b"\0\0\0\0" + good[4:],
            "540": good[:540] + (1000).to_bytes(4, "little") + good[544:],
            "length": good[:1392] + (len(self.picture) - 1).to_bytes(4, "little") + good[1396:],
            "trailing": good + b"\0",
            "no jpeg": good[:1396] + b"\0" * len(self.picture),
            "name": good[:684] + b"\x01bad" + good[688:],
            "short": good[:1000],
        }
        for what, data in spoilt.items():
            with self.subTest(what):
                with self.assertRaisesRegex(oot.Fail, "not the caption.sav layout understood"):
                    oot.caption_layout(data)

    # -- scrub --

    def test_scrub_without_a_name_field_notes_there_is_none(self):
        """This game's saves show no player name (the Load screen: picture, date, place, hearts): no field to set."""
        code, out, err = self.run_main("scrub", str(self.src), "--slot", "02", "--out", str(self.saves / "kokiri-start"))
        self.assertEqual(code, 0, err)
        self.assertIn("no player name to set", out)
        for f in ("progress.sav", "caption.sav"):
            self.assertEqual(int((self.saves / "kokiri-start" / "slot_00" / f).stat().st_mtime), oot.SCRUB_TIME, f)

    def test_scrub(self):
        code, out, err = self.scrub()
        self.assertEqual(code, 0, err)
        dest = self.saves / "kokiri-start"
        self.assertEqual(sorted(str(p.relative_to(dest)) for p in dest.rglob("*")),
                         ["README.md", "slot_00", os.path.join("slot_00", "caption.sav"),
                          os.path.join("slot_00", "progress.sav")])
        progress = (dest / "slot_00" / "progress.sav").read_bytes()
        self.assertEqual(progress, PROGRESS[:NAME_AT] + b"FARM\0\0\0\0" + PROGRESS[NAME_AT + 8:])
        new = (dest / "slot_00" / "caption.sav").read_bytes()
        old = caption(self.picture)
        dated = bytearray(old[:1392])
        for field, at in DATE_AT.items():
            dated[at + 4:at + 8] = oot.SCRUB_DATE[field].to_bytes(4, "little")
        self.assertEqual(new[:1392], bytes(dated))   # (the header kept, but for the date and the JPEG's length)
        self.assertEqual(new[HEALTH_AT:HEALTH_AT + 8], old[HEALTH_AT:HEALTH_AT + 8])
        self.assertEqual({f: v for f, (_, v) in oot.caption_dates(new).items()},
                         {"year": 2026, "month": 1, "day": 1, "hour": 0, "minute": 0, "second": 0})
        for field, at in DATE_AT.items():
            self.assertIn("caption.sav: the save date (%s at %d): %d -> %d" % (field, at, DATE[field],
                                                                              oot.SCRUB_DATE[field]), out)
        self.assertIn("the files' own; the Load screen's date is caption.sav's", out)
        self.assertNotIn("(the Load screen's date)", out)
        grey = oot.grey_jpeg(543, 306)
        self.assertEqual(new[1392:], len(grey).to_bytes(4, "little") + grey)
        self.assertIsNone(oot.jpeg_is_mid_grey(oot.caption_layout(new)["jpeg"]))
        self.assertIn("player name at 100 (8 bytes, ascii): 504c415945520000 -> 4641524d00000000 (FARM)", out)
        self.assertIn("the picture at 1396 (JPEG 543x306, %d bytes) replaced by a mid-grey JPEG 543x306 of %d bytes"
                      % (len(self.picture), len(grey)), out)
        self.assertIn("the JPEG's length at 1392: %d -> %d" % (len(self.picture), len(grey)), out)
        self.assertIn("scene TestScene, map TestScene/Room_1", out)
        self.assertIn("README.md: written from the template", out)
        self.assertIn("first line is still the template's", " ".join(oot.repo_save_problems(str(self.saves))))
        # (the source only read)
        self.assertEqual((self.src / "slot_02" / "progress.sav").read_bytes(), PROGRESS)
        self.assertEqual((self.src / "slot_02" / "caption.sav").read_bytes(), old)
        code, out, err = self.scrub()
        self.assertEqual(code, 1)
        self.assertIn("already holds a save", err)

    def test_scrub_refusals(self):
        cases = []
        cases.append(("bad name", lambda: None, ("Kokiri Start",), "--out: a repo save's folder"))
        cases.append(("profile", lambda: None, ("_profile",), "--out: a repo save's folder"))
        cases.append(("no slot", lambda: None, ("x", "--slot", "07"), "has no slot_07"))
        cases.append(("extra file", lambda: (self.src / "slot_02" / "notes.txt").write_bytes(b"x"), ("x",),
                      "notes.txt besides"))
        cases.append(("layout", lambda: (self.src / "slot_02" / "caption.sav").write_bytes(caption(self.picture) + b"\0"),
                      ("x",), "not the caption.sav layout understood"))
        cases.append(("huge picture", lambda: (self.src / "slot_02" / "caption.sav").write_bytes(
            caption(oot.grey_jpeg(8, 8).replace(b"\x00\x08\x00\x08\x03", b"\x10\x00\x10\x00\x03", 1))), ("x",),
            "larger than a file-select picture"))
        def dates(**kw):
            return lambda: (self.src / "slot_02" / "caption.sav").write_bytes(caption(self.picture, **kw))
        cases.append(("no day", dates(date_at={f: o for f, o in DATE_AT.items() if f != "day"}), ("x",),
                      "the date's day key 0xbd46f485 is not in the header"))
        cases.append(("two minutes", dates(more=[("minute", 400)]), ("x",), "the date's minute key 0x27853bf7 is 2 times"))
        cases.append(("key in the picture's length", dates(date_at=dict(DATE_AT, year=1388)), ("x",),
                      "the date's year key 0x9811a3f7 is not whole"))
        cases.append(("off the grid", dates(date_at=dict(DATE_AT, hour=332)), ("x",), "not 8-byte aligned"))
        cases.append(("long name", lambda: None, ("x", "--name", "FARMFARMFARM"), "--name: up to 8"))
        cases.append(("field", lambda: None, ("x", "--name-field", "account.sav:0:8"), "--name-field FILE"))
        if os.name != "nt":
            cases.append(("link", lambda: os.symlink(self.tmp / "secret", self.src / "slot_02" / "secret"), ("x",),
                          "is a link"))
        for what, spoil, args, why in cases:
            with self.subTest(what):
                backup = self.tmp / "keep"
                shutil.copytree(self.src, backup)
                spoil()
                out_name, rest = args[0], list(args[1:])
                argv = ["scrub", str(self.src), "--slot", "02", "--out", str(self.saves / out_name),
                        "--name-field", "progress.sav:%d:8" % NAME_AT]
                if "--slot" in rest:
                    argv[3] = rest[rest.index("--slot") + 1]
                    rest = []
                code, out, err = self.run_main(*(argv + rest))
                self.assertEqual(code, 1, out)
                self.assertIn(why, err)
                self.assertFalse((self.saves / out_name).exists())
                shutil.rmtree(self.src)
                backup.rename(self.src)

    # -- check-repo-saves --

    def test_check_passes(self):
        self.passing()
        code, out, err = self.run_main("check-repo-saves", str(self.saves))
        self.assertEqual(code, 0, out)
        self.assertIn("passes: 1 repo save(s)", out)
        (self.saves / ".gitkeep").write_bytes(b"")
        self.assertEqual(oot.repo_save_problems(str(self.saves)), [])

    def test_check_refusals(self):
        self.passing()
        save = self.saves / "kokiri-start"
        cases = [
            ("extra in save", lambda: (save / "notes.txt").write_text("x"), "kokiri-start/notes.txt does not belong"),
            ("extra in slot", lambda: (save / "slot_00" / "x.bin").write_bytes(b"x"), "slot_00/x.bin does not belong"),
            ("second slot", lambda: shutil.copytree(save / "slot_00", save / "slot_01"), "slot_01 does not belong"),
            ("settings", lambda: (save / "slot_00" / "hookshot_options.ini").write_text("x"),
             "hookshot_options.ini does not belong"),
            ("top level", lambda: (self.saves / "hookshot_env.txt").write_text("x"), "hookshot_env.txt does not belong"),
            ("folder name", lambda: shutil.copytree(save, self.saves / "Bad Name"), "Bad Name does not belong"),
            ("gitkeep", lambda: (self.saves / ".gitkeep").write_text("x"), ".gitkeep does not belong"),
            ("profile", lambda: (self.saves / "_profile").mkdir() or (self.saves / "_profile" / "account.sav").write_bytes(
                b"someone's account"), "_profile/ is not shipped any more"),
            ("profile's account", lambda: (self.saves / "_profile").mkdir() or (self.saves / "_profile" / "account.sav")
             .write_bytes(b"someone's account"), "_profile/account.sav: account.sav is made by the game"),
            ("empty profile", lambda: (self.saves / "_profile").mkdir(), "_profile/ is not shipped any more"),
            ("stray account", lambda: (save / "slot_00" / "account.sav").write_bytes(b"x"),
             "kokiri-start/slot_00/account.sav: account.sav is made by the game"),
            ("stray option", lambda: (save / "option.sav").write_bytes(b"x"),
             "kokiri-start/option.sav: option.sav is made by the game"),
            ("top account", lambda: (self.saves / "account.sav").write_bytes(b"x"),
             "account.sav: account.sav is made by the game"),
            ("date", lambda: (save / "slot_00" / "caption.sav").write_bytes(caption(oot.grey_jpeg(32, 32))),
             "the save date is not 2026-01-01 00:00:00 (month 11, day 14, hour 9, minute 41, second 7): not scrubbed"),
            ("year", lambda: (save / "slot_00" / "caption.sav").write_bytes(caption(oot.grey_jpeg(32, 32), date=dict(
                oot.SCRUB_DATE, year=2025))), "(year 2025): not scrubbed"),
            ("no date", lambda: (save / "slot_00" / "caption.sav").write_bytes(caption(oot.grey_jpeg(32, 32), date_at={})),
             "the date's year key 0x9811a3f7 is not in the header"),
            ("no readme", lambda: (save / "README.md").unlink(), "kokiri-start/README.md is missing"),
            ("top readme", lambda: (self.saves / "README.md").unlink(), "README.md is missing"),
            ("empty readme", lambda: (save / "README.md").write_text("\n\n"), "has no first line"),
            ("empty file", lambda: (save / "slot_00" / "progress.sav").write_bytes(b""), "progress.sav is empty"),
            ("large", lambda: (save / "slot_00" / "progress.sav").write_bytes(b"\0" * (oot.REPO_CAPS["progress.sav"] + 1)),
             "over the"),
            ("picture", lambda: (save / "slot_00" / "caption.sav").write_bytes(caption(oot.grey_jpeg(32, 32, 90))),
             "not flat mid-grey"),
            ("real-sized picture", lambda: (save / "slot_00" / "caption.sav").write_bytes(caption(self.picture)),
             "not flat mid-grey"),
            ("layout", lambda: (save / "slot_00" / "caption.sav").write_bytes(caption(oot.grey_jpeg(8, 8)) + b"\0"),
             "not the caption.sav layout understood"),
        ]
        if os.name != "nt":
            cases.append(("link", lambda: (save / "slot_00" / "progress.sav").unlink() or
                          os.symlink(self.tmp / "secret", save / "slot_00" / "progress.sav"),
                          "slot_00/progress.sav is a link"))
            cases.append(("linked save", lambda: os.symlink(save, self.saves / "linked"), "linked is a link"))
        for what, spoil, why in cases:
            with self.subTest(what):
                keep = self.tmp / "keep"
                shutil.copytree(self.saves, keep, symlinks=True)
                spoil()
                code, out, err = self.run_main("check-repo-saves", str(self.saves))
                self.assertEqual(code, 1, out)
                self.assertIn(why, out)
                shutil.rmtree(self.saves)
                keep.rename(self.saves)
        self.assertEqual(oot.repo_save_problems(str(self.saves)), [])

    def test_the_checkouts_repo_saves_pass(self):
        # (whatever assets/saves holds in this checkout: every save added there is checked with the other tests)
        self.assertEqual(oot.repo_save_problems(oot.REPO_SAVES), [])

if __name__ == "__main__":
    unittest.main()
