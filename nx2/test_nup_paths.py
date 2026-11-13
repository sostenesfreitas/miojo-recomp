"""Game-free extraction boundary regressions: python -I nx2/test_nup_paths.py."""

from contextlib import contextmanager, ExitStack, redirect_stdout
import errno
import importlib.util
import io
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nup


class WriteAttempt(AssertionError):
    pass


@contextmanager
def guard_io(root):
    """Keep unsafe probes inside this fixture, without faking successful metadata reads."""
    def normalized(path):
        path = os.path.normcase(os.path.abspath(path))
        # Compare equivalent filesystem paths; keep the original spelling for the real metadata call.
        if os.name == "nt":
            if path.startswith("\\\\?\\unc\\"):
                return "\\\\" + path[8:]
            if path.startswith("\\\\?\\") and path[5:7] == ":\\":
                return path[4:]
        return path

    roots = {normalized(root), normalized(os.path.realpath(root))}
    attempts = []

    def metadata(function):
        def checked(path, *args, **kwargs):
            candidate = normalized(path)
            for owned in roots:
                try:
                    common = os.path.commonpath((candidate, owned))
                except ValueError:
                    continue
                if common in (candidate, owned):   # realpath may inspect the fixture's ancestors
                    return function(path, *args, **kwargs)
            raise AssertionError("metadata access outside fixture: %r" % path)
        return checked

    def writing(name):
        def blocked(*args, **kwargs):
            attempts.append(name)
            raise WriteAttempt(name)
        return blocked

    with ExitStack() as stack:
        for name in ("stat", "lstat"):
            stack.enter_context(patch.object(os, name, side_effect=metadata(getattr(os, name))))
        for name in ("makedirs", "mkdir", "open", "replace", "rename", "renames", "link", "symlink",
                     "unlink", "remove", "rmdir", "removedirs"):
            stack.enter_context(patch.object(os, name, side_effect=writing("os." + name)))
        stack.enter_context(patch("builtins.open", side_effect=writing("builtins.open")))
        stack.enter_context(patch.object(io, "open", side_effect=writing("io.open")))
        yield attempts


def snapshot(root):
    """Record directories and link targets too; never descend into a link or junction."""
    result = {}
    pending = [root]
    while pending:
        for path in pending.pop().iterdir():
            info = path.lstat()
            relative = path.relative_to(root)
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
                result[relative] = ("link", os.readlink(path))
            elif stat.S_ISDIR(info.st_mode):
                result[relative] = ("directory",)
                pending.append(path)
            else:
                result[relative] = ("file", path.read_bytes())
    return result


class Package:
    def __init__(self, names, data=b"authored fixture"):
        self.names, self.data = names, data

    def walk(self, directory):
        for name in self.names:
            yield name, (0, 0, len(self.data), 0, len(self.data))

    def copy_to(self, record, output, name):
        output.write(self.data)


class HealingPackage(Package):
    """The selected update is missing main; the base has an authored module."""
    titles = [dict(index=0, id=0, version=0, exefs=0, romfs=1),
              dict(index=1, id=0, version=1, exefs=2, romfs=3)]
    file_size, nd, nf = 0, 4, 1

    def __init__(self, data=b"NSO0authored module"):
        super().__init__(["main"], data)

    def walk(self, directory):
        if directory == 0:
            yield from super().walk(directory)

    def read(self, record, n=None):
        return self.data if n is None else self.data[:n]


class ExtractionPaths(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.out = self.root / "out"
        self.out.mkdir()
        self.sentinel = self.root / "outside.bin"
        self.sentinel.write_bytes(b"keep outside data")

    def extract(self, names):
        return nup.extract(Package(names), {"romfs": 0}, str(self.out), only="romfs", quiet=True)

    def test_cli_measures_space_at_the_extraction_destination(self):
        package = Package(["authored.bin"])
        package.titles = [dict(index=0, id=0, version=1, exefs=0, romfs=1)]
        package.file_size, package.nd, package.nf = 0, 2, 1
        for destination in (self.out, self.out / "new" / "nested"):
            with self.subTest(destination=destination), \
                    patch.object(nup, "Nup", return_value=package), \
                    patch.object(nup.shutil, "disk_usage", wraps=nup.shutil.disk_usage) as usage, \
                    patch.object(sys, "argv", ["nup.py", "authored.nup", "--extract", str(destination), "--only", "romfs"]), \
                    redirect_stdout(io.StringIO()):
                nup.main()
                usage.assert_called_once_with(str(self.out.resolve()))
                self.assertEqual((destination / "romfs" / "authored.bin").read_bytes(), package.data)

    def heal(self):
        package = HealingPackage()
        with redirect_stdout(io.StringIO()):
            nup.heal(package, package.titles[-1], str(self.out), ["main"])

    def unpack(self):
        # This authored empty-segment NSO exercises the real unpack writer, without compressed game data.
        raw = bytearray(0x100)
        raw[:4] = b"NSO0"
        struct.pack_into("<I", raw, 0xC, 1 << 7)
        for i in range(3):
            struct.pack_into("<I", raw, 0x10 + 0x10 * i, 0x100)
        (self.out / "exefs" / "main").write_bytes(raw)
        spec = importlib.util.spec_from_file_location("unpack_fixture", Path(nup.__file__).with_name("unpack.py"))
        unpacker = importlib.util.module_from_spec(spec)
        # No compressed segment may call the project's native decompressor in this fixture.
        with patch.dict(sys.modules, {"zbic": object()}):
            spec.loader.exec_module(unpacker)
        unpacker.NAMES = ["main"]
        with patch.object(nup, "Nup", return_value=HealingPackage(bytes(raw))), \
                patch.object(sys, "argv", ["nup.py", "authored.nup", "--extract", str(self.out)]), \
                patch.dict(sys.modules, {"unpack": unpacker}), redirect_stdout(io.StringIO()):
            return nup.main()

    def reject(self, names, message):
        before = snapshot(self.root)
        with self.assertRaisesRegex(SystemExit, "^" + re.escape(message)):
            self.extract(names)
        self.assertEqual(before, snapshot(self.root))

    def directory_link(self, link, target):
        try:
            os.symlink(target, link, target_is_directory=True)
        except OSError as error:
            if os.name != "nt":
                raise
            # Junctions need no Developer Mode or symbolic-link privilege on Windows.
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                                    capture_output=True, text=True)
            if result.returncode:
                self.fail("Cannot create the link fixture: %s; %s" % (error, result.stderr))

    def test_nested_names_and_resume(self):
        names = ["Map/Forest/data.bin", "Text/Français.bin", "com10.txt", ".hidden", "conifer.txt"]
        self.assertEqual(self.extract(names), (len(names), len(names) * len(b"authored fixture")))
        for name in names:
            self.assertEqual((self.out / "romfs" / name).read_bytes(), b"authored fixture")
        self.assertEqual(self.extract(names), (0, 0))

    def test_paths_cannot_leave_the_output(self):
        names = ["", ".", "..", "../../outside.bin", "nested/../../outside.bin", "nested/../file.bin",
                 "/rooted.bin", "//server/share/file.bin", r"\rooted.bin", r"\\server\share\file.bin",
                 r"C:\outside.bin", "C:/outside.bin", "C:outside.bin", r"nested\..\outside.bin",
                 "nested//file.bin", "nested/.. /outside.bin", "file:stream", "trailing.", "trailing ",
                 "control\x00.bin", "CON", "aux.txt", "NUL.bin", "COM1", "LPT9.txt", "COM¹.txt",
                 "CONIN$", "conout$", "nested/CONIN$", "nested/ConOut$"]
        for name in names:
            with self.subTest(name=name):
                # Even a broken validator must not let the fixture touch an absolute path or UNC share.
                before = snapshot(self.root)
                with guard_io(self.root) as attempts:
                    with self.assertRaisesRegex(SystemExit, "^" + re.escape("unsafe path in the package: %r" % name) + "$"):
                        self.extract([name])
                    self.assertEqual(attempts, [])
                    # In this exact context, a valid name must pass validation and reach a write.
                    with self.assertRaisesRegex(WriteAttempt, "^os\\.makedirs$"):
                        self.extract(["valid.bin"])
                    self.assertEqual(attempts, ["os.makedirs"])
                self.assertEqual(before, snapshot(self.root))

    def test_write_guards_block_alternative_io(self):
        before = snapshot(self.root)
        operations = (("builtins.open", lambda: open(self.sentinel, "wb")),
                      ("io.open", lambda: io.open(self.sentinel, "wb")),
                      ("os.open", lambda: os.open(self.sentinel, os.O_WRONLY | os.O_TRUNC)),
                      ("os.mkdir", lambda: os.mkdir(self.root / "new")),
                      ("os.rename", lambda: os.rename(self.sentinel, self.root / "renamed")),
                      ("os.link", lambda: os.link(self.sentinel, self.root / "linked")))
        with guard_io(self.root) as attempts:
            for name, operation in operations:
                with self.subTest(operation=name), self.assertRaisesRegex(WriteAttempt, "^" + re.escape(name) + "$"):
                    result = operation()
                    if name == "os.open":
                        os.close(result)
                    elif hasattr(result, "close"):
                        result.close()
            self.assertEqual(attempts, [name for name, _ in operations])
        self.assertEqual(before, snapshot(self.root))

    def test_metadata_guards_accept_owned_paths_and_reject_siblings(self):
        outside = self.root.with_name(self.root.name + "-sibling") / "outside.bin"
        with guard_io(self.root) as attempts:
            for path in dict.fromkeys((str(self.sentinel), nup.long_path(str(self.sentinel)))):
                with self.subTest(owned=path):
                    self.assertEqual(os.lstat(path).st_size, len(b"keep outside data"))
            for path in dict.fromkeys((str(outside), nup.long_path(str(outside)))):
                with self.subTest(outside=path), self.assertRaisesRegex(AssertionError, "^metadata access outside fixture:"):
                    os.lstat(path)
            self.assertEqual(attempts, [])

    def test_parent_path_cannot_overwrite_a_sibling(self):
        # This also safely demonstrates the original failure: both files belong to this fixture.
        self.reject(["../../outside.bin"], "unsafe path in the package: '../../outside.bin'")

    def test_ordinary_partial_file_is_replaced(self):
        (self.out / "romfs").mkdir()
        partial = self.out / "romfs" / "file.bin.part"
        partial.write_bytes(b"an interrupted extraction")
        self.extract(["file.bin"])
        self.assertEqual((self.out / "romfs" / "file.bin").read_bytes(), b"authored fixture")
        self.assertFalse(partial.exists())

    def test_failed_member_is_removed_and_can_be_retried(self):
        target = self.out / "romfs" / "file.bin"
        target.parent.mkdir()
        package = Package(["file.bin"])
        title = {"romfs": 0}

        def failing(error):
            def copy_to(record, output, name):
                output.write(b"some bytes")
                raise error
            return copy_to

        for error in (OSError(errno.ENOSPC, "authored disk-full failure"), KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                target.write_bytes(b"previous file")
                with patch.object(package, "copy_to", side_effect=failing(error)), self.assertRaises(type(error)):
                    nup.extract(package, title, str(self.out), only="romfs", quiet=True)
                self.assertFalse(target.exists())
                self.assertEqual(nup.extract(package, title, str(self.out), only="romfs", quiet=True),
                                 (1, len(package.data)))
                self.assertEqual(target.read_bytes(), package.data)

    def test_member_cut_short_is_written_again(self):
        # A killed extraction leaves its last member shorter than the package says.
        names = ["a/first.bin", "a/second.bin", "b/third.bin"]
        self.extract(names)
        cut = self.out / "romfs" / "a" / "second.bin"
        cut.write_bytes(b"authored")
        partial = cut.with_name("second.bin.part")   # (as builders that wrote a temporary left it)
        partial.write_bytes(b"authored")
        (self.out / "romfs" / "b" / "third.bin").unlink()
        self.assertEqual(self.extract(names), (2, 2 * len(b"authored fixture")))
        self.assertFalse(partial.exists())
        for name in names:
            self.assertEqual((self.out / "romfs" / name).read_bytes(), b"authored fixture")

    def test_members_are_written_in_place_and_folders_checked_once(self):
        names = ["Map/%d/file%d.bin" % (i % 3, i) for i in range(30)]
        with patch.object(nup.os, "replace", wraps=os.replace) as replace,                 patch.object(nup.os, "lstat", wraps=os.lstat) as lstat:
            self.assertEqual(self.extract(names)[0], len(names))
        self.assertEqual([os.path.basename(call.args[1]) for call in replace.call_args_list], [nup.HEADS_NAME])
        romfs = os.path.normcase(nup.long_path(os.path.realpath(self.out / "romfs")))
        under = [call for call in lstat.call_args_list
                 if os.path.normcase(nup.long_path(str(call.args[0]))).startswith(romfs)]
        self.assertEqual(len(under), 1 + 1 + 3 + len(names))   # romfs, Map, Map/0..2, then each member once
        self.assertEqual(sorted(p.name for p in (self.out / "romfs").rglob("*") if p.is_file()),
                         sorted(name.split("/")[-1] for name in names))

    def test_destination_hard_link_is_replaced_without_truncation(self):
        (self.out / "romfs").mkdir()
        member = self.out / "romfs" / "file.bin"
        os.link(self.sentinel, member)
        self.extract(["file.bin"])
        self.assertEqual(self.sentinel.read_bytes(), b"keep outside data")
        self.assertEqual(member.read_bytes(), b"authored fixture")

    def test_part_directory_link_cannot_redirect_extraction(self):
        outside = self.root / "outside"
        outside.mkdir()
        self.directory_link(self.out / "romfs", outside)
        self.reject(["new.bin"], "extraction path is a link or reparse point: ")

    def test_nested_directory_link_cannot_redirect_extraction(self):
        (self.out / "romfs").mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        self.directory_link(self.out / "romfs" / "nested", outside)
        self.reject(["nested/new.bin"], "extraction path is a link or reparse point: ")

    def test_directory_reparse_point_cannot_redirect_extraction(self):
        outside = self.root / "outside"
        outside.mkdir()
        link = self.out / "romfs"
        if os.name == "nt":
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            os.symlink(outside, link, target_is_directory=True)
        self.reject(["new.bin"], "extraction path is a link or reparse point: ")

    def test_destination_link_is_rejected_before_resume(self):
        (self.out / "romfs").mkdir()
        self.sentinel.write_bytes(b"authored fixture")
        try:
            os.symlink(self.sentinel, self.out / "romfs" / "file.bin")
        except OSError as error:
            self.fail("Cannot create the file-link fixture: %s" % error)
        self.reject(["file.bin"], "extraction path is a link or reparse point: ")

    def test_stale_temporary_link_cannot_truncate_an_outside_file(self):
        (self.out / "romfs").mkdir()
        try:
            os.symlink(self.sentinel, self.out / "romfs" / "file.bin.part")
        except OSError as error:
            self.fail("Cannot create the file-link fixture: %s" % error)
        self.extract(["file.bin"])
        self.assertEqual(self.sentinel.read_bytes(), b"keep outside data")
        self.assertFalse(os.path.lexists(self.out / "romfs" / "file.bin.part"))
        self.assertEqual((self.out / "romfs" / "file.bin").read_bytes(), b"authored fixture")

    def test_stale_temporary_hard_link_cannot_truncate_an_outside_file(self):
        (self.out / "romfs").mkdir()
        os.link(self.sentinel, self.out / "romfs" / "file.bin.part")
        self.extract(["file.bin"])
        self.assertEqual(self.sentinel.read_bytes(), b"keep outside data")
        self.assertFalse((self.out / "romfs" / "file.bin.part").exists())
        self.assertEqual((self.out / "romfs" / "file.bin").read_bytes(), b"authored fixture")

    def test_heal_missing_module_and_resume(self):
        (self.out / "exefs").mkdir()
        self.heal()
        self.assertEqual((self.out / "exefs" / "main").read_bytes(), b"NSO0authored module")
        before = snapshot(self.root)
        self.heal()
        self.assertEqual(before, snapshot(self.root))

    def test_heal_directory_link_cannot_redirect_module(self):
        outside = self.root / "outside"
        outside.mkdir()
        self.directory_link(self.out / "exefs", outside)
        before = snapshot(self.root)
        with self.assertRaisesRegex(SystemExit, "^extraction path is a link or reparse point: "):
            self.heal()
        self.assertEqual(before, snapshot(self.root))

    def test_heal_directory_reparse_point_cannot_redirect_module(self):
        outside = self.root / "outside"
        outside.mkdir()
        link = self.out / "exefs"
        if os.name == "nt":
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            os.symlink(outside, link, target_is_directory=True)
        before = snapshot(self.root)
        with self.assertRaisesRegex(SystemExit, "^extraction path is a link or reparse point: "):
            self.heal()
        self.assertEqual(before, snapshot(self.root))

    def test_heal_destination_link_cannot_truncate_an_outside_file(self):
        (self.out / "exefs").mkdir()
        os.symlink(self.sentinel, self.out / "exefs" / "main")
        before = snapshot(self.root)
        with self.assertRaisesRegex(SystemExit, "^extraction path is a link or reparse point: "):
            self.heal()
        self.assertEqual(before, snapshot(self.root))

    def test_heal_destination_hard_link_is_replaced_without_truncation(self):
        (self.out / "exefs").mkdir()
        module = self.out / "exefs" / "main"
        os.link(self.sentinel, module)
        self.heal()
        self.assertEqual(self.sentinel.read_bytes(), b"keep outside data")
        self.assertEqual(module.read_bytes(), b"NSO0authored module")

    def test_heal_temporary_link_cannot_truncate_an_outside_file(self):
        (self.out / "exefs").mkdir()
        os.symlink(self.sentinel, self.out / "exefs" / "main.part")
        before = snapshot(self.root)
        with self.assertRaisesRegex(SystemExit, "^extraction path is a link or reparse point: "):
            self.heal()
        self.assertEqual(before, snapshot(self.root))

    def test_heal_temporary_hard_link_cannot_truncate_an_outside_file(self):
        (self.out / "exefs").mkdir()
        os.link(self.sentinel, self.out / "exefs" / "main.part")
        before = snapshot(self.root)
        with self.assertRaisesRegex(SystemExit, "^extraction temporary file has other hard links: "):
            self.heal()
        self.assertEqual(before, snapshot(self.root))

    def test_unpack_temporary_link_cannot_truncate_an_outside_file(self):
        (self.out / "exefs").mkdir()
        os.symlink(self.sentinel, self.out / "exefs" / "main.plain")
        # unpack() creates only the authored module before entering nup.main's production path.
        with self.assertRaisesRegex(SystemExit, r"^extraction path is a link or reparse point: .*main\.plain$"):
            self.unpack()
        self.assertEqual(self.sentinel.read_bytes(), b"keep outside data")
        self.assertTrue((self.out / "exefs" / "main.plain").is_symlink())

    def test_unpack_temporary_hard_link_cannot_truncate_an_outside_file(self):
        (self.out / "exefs").mkdir()
        os.link(self.sentinel, self.out / "exefs" / "main.plain")
        with self.assertRaisesRegex(SystemExit, r"^extraction temporary file has other hard links: .*main\.plain$"):
            self.unpack()
        self.assertEqual(self.sentinel.read_bytes(), b"keep outside data")

    def test_unpack_ordinary_temporary_file_is_replaced(self):
        (self.out / "exefs").mkdir()
        (self.out / "exefs" / "main.plain").write_bytes(b"interrupted unpack")
        self.unpack()
        module = (self.out / "exefs" / "main").read_bytes()
        self.assertEqual(module[:4], b"NSO0")
        self.assertEqual(struct.unpack_from("<I", module, 0xC)[0], 0)
        self.assertFalse((self.out / "exefs" / "main.plain").exists())


if __name__ == "__main__":
    unittest.main()
