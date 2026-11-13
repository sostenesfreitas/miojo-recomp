"""scripts/oot_saves.py, game-free: finding every emulator's folders, importing and exporting, the backups, the guards.
Temp folders only: HOME, XDG_DATA_HOME, APPDATA and LOCALAPPDATA all point into one.

python -I -B tests/test_oot_saves.py
"""
import contextlib
import importlib.util
import io
import os
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
        expect = {"lichii": self.li, "frizti": self.li, "orchid": self.li, "pokedinner": self.li, "eden": self.li,
                  "eglantine": eden_save(self.data / "eglantine"), "nx2navi": nx}
        self.assertEqual(sorted(expect), sorted(p.name for p in oot.EMULATORS))
        for program in oot.EMULATORS:
            folder, found = oot.locate(program, None, "--from")
            self.assertEqual(list(found.values()), [str(expect[program.name])], program.name)
        self.assertEqual(oot.locate(oot.HOOKSHOT, None, "--hookshot")[1], {UID: str(self.hs)})

    def test_every_entry_on_windows(self):
        win_li = eden_save(self.appdata / "eden")
        win_eg = eden_save(self.appdata / "eglantine")
        win_nx = self.localappdata / "Programs" / "NX2navi" / "game" / "profile" / "save" / ACCOUNT
        for folder in (win_li, win_eg, win_nx):
            make_save(folder, b"W")
        expect = {"lichii": win_li, "frizti": win_li, "orchid": win_li, "pokedinner": win_li, "eden": win_li,
                  "eglantine": win_eg, "nx2navi": win_nx}
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


if __name__ == "__main__":
    unittest.main()
