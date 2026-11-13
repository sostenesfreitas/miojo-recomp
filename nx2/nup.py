"""Read a .nup package (a decrypted Switch 2 dump: titles, each with an exefs/ and a romfs/ tree) and extract it.

    python -I nx2/nup.py <file.nup>                      list the titles and their trees
    python -I nx2/nup.py <file.nup> --verify            read-only hashes/extents and expected game .zs headers
    python -I nx2/nup.py <file.nup> --extract <dir> [--title <n|version>] [--only exefs|romfs]

The layout (worked out from the file; little-endian):
    0x000  "NUP\\x01" header, u64 file size at +0x40
    0x080  "TOC\\x01", u32 titles, u64 dirs, u64 files, u64 strings offset (from 0x80), u64 strings size
    0x0b0  titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version, ...,
           u64 offset + u64 size of the title's metadata blob at +0x20
    then   dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 children, u64 0
    then   files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size,
           32-byte hash, u8 codec (0 raw, 1 zstd), u8 flags, u16 reserved, 12 bytes reserved
    then   the child list, u32 each: a node index; below the dir count a dir, else (index - dirs) a file
    then   the name strings (not 0-terminated)
The data of a file is stored as is at its data offset, or, in .nup files from newer dumpers, as a zstd frame whose content is the
file: its size field is then the size of the file, not of the frame (seen on rtld: 10524 bytes in the listing, frame content 10524).
A file that is itself zstd in the game (the romfs .zs files) is not one of those: its frame's content is larger than the file.
"""
import argparse
from contextlib import suppress
import errno
import hashlib
import os
import re
import shutil
import stat
import struct
import sys
from time import monotonic


ZSTD = b"\x28\xb5\x2f\xfd"
WINDOWS_DEVICES = {"CON", "CONIN$", "CONOUT$", "PRN", "AUX", "NUL"} | {p + n for p in ("COM", "LPT") for n in "123456789¹²³"}


def frame_content_size(head):
    """The content size a zstd frame header states, None when it states none"""
    if head[:4] != ZSTD or len(head) < 18:
        return None
    fhd = head[4]
    single = (fhd >> 5) & 1
    at = 5 + (0 if single else 1) + (0, 1, 2, 4)[fhd & 3]
    n = (1 if single else 0, 2, 4, 8)[fhd >> 6]
    if not n:
        return None
    size = int.from_bytes(head[at:at + n], "little")
    return size + 256 if n == 2 else size


class Nup:
    def __init__(self, path, *, strict=False):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0x80)
        if strict and len(h) != 0x80:
            self.f.close()
            raise ValueError("truncated NUP header")
        if h[:4] != b"NUP\x01":
            raise SystemExit("%s: not a NUP package (magic %r)" % (path, h[:4]))
        self.file_size = struct.unpack_from("<Q", h, 0x40)[0]
        self.toc_offset, self.toc_size = struct.unpack_from("<QQ", h, 8)
        self.toc_sha256 = h[24:56]
        self.blob_offset = struct.unpack_from("<Q", h, 0x38)[0]
        # (a package that ends early (a download or copy that did not finish) failed 30 minutes in as "short read in the
        # package": said now, with the sizes)
        have = os.fstat(self.f.fileno()).st_size
        if strict:
            try:
                self._verify_toc()   # check metadata hash/bounds before parsing records
            except (ValueError, OSError, struct.error):
                self.f.close()
                raise
            self.f.seek(0x80)
        if self.file_size and have < self.file_size:
            raise SystemExit("the .nup file is incomplete: it is %d MB, its header says %d MB. Download or copy it again "
                             "(wait for the download to finish) and check that the drive had room" % (have >> 20, self.file_size >> 20))
        t = self.f.read(0x30)
        if t[:4] != b"TOC\x01":
            raise SystemExit("%s: no TOC at 0x80" % path)
        nt, nd, nf, soff, ssz = struct.unpack_from("<IQQQQ", t, 4)
        self.nd, self.nf = nd, nf
        dirs_at = 0x80 + 0x30 + 0x40 * nt
        files_at = dirs_at + 24 * nd
        kids_at = files_at + 80 * nf
        strings_at = 0x80 + soff
        self.f.seek(0)
        meta = self.f.read(strings_at + ssz)
        self.strings = meta[strings_at:strings_at + ssz]
        self.titles = []
        for i in range(nt):
            o = 0xb0 + 0x40 * i
            tid, root, exefs, romfs, ver = struct.unpack_from("<QIIII", meta, o)
            self.titles.append(dict(index=i, id=tid, root=root, exefs=exefs, romfs=romfs, version=ver))
        self.dirs = [struct.unpack_from("<4I", meta, dirs_at + 24 * i) for i in range(nd)]
        self.files = []
        self.file_details = []   # hash, codec flags and reserved bytes; keep files tuples compatible
        for i in range(nf):
            no, nl, size, off, size2 = struct.unpack_from("<IIQQQ", meta, files_at + 80 * i)
            self.files.append((no, nl, size, off, size2, meta[files_at + 80 * i + 64]))
            at = files_at + 80 * i
            self.file_details.append((meta[at + 32:at + 64], meta[at + 65], meta[at + 66:at + 80]))
        nk = (strings_at - kids_at) // 4
        self.kids = struct.unpack_from("<%dI" % nk, meta, kids_at)
        if strict:
            try:
                self._verify_tree()
            except ValueError:
                self.f.close()
                raise

    def _verify_toc(self):
        """Check parsed metadata bounds and the complete TOC hash."""
        self.f.seek(0)
        h = self.f.read(0x80)
        physical = os.fstat(self.f.fileno()).st_size
        if (len(h) != 0x80 or self.file_size != physical or self.toc_offset != 0x80 or
                self.toc_size < 0x30 or self.toc_offset + self.toc_size > self.blob_offset or
                self.blob_offset > physical or self.blob_offset % 16):
            raise ValueError("invalid declared file/TOC/blob bounds")
        self.f.seek(self.toc_offset)
        t = self.f.read(0x30)
        if len(t) != 0x30 or t[:4] != b"TOC\x01":
            raise ValueError("invalid TOC header")
        nt, nd, nf, soff, ssz = struct.unpack_from("<IQQQQ", t, 4)
        tables_end = 0x30 + 64 * nt + 24 * nd + 80 * nf
        if (not nt or tables_end > soff or (soff - tables_end) % 4 or
                soff + ssz > self.toc_size):
            raise ValueError("parsed title/directory/file/child/string tables exceed declared TOC")
        self.f.seek(self.toc_offset)
        left, digest = self.toc_size, hashlib.sha256()
        while left:
            block = self.f.read(min(left, 8 << 20))
            if not block:
                raise ValueError("truncated TOC")
            left -= len(block)
            digest.update(block)
        if digest.digest() != self.toc_sha256:
            raise ValueError("TOC SHA-256 mismatch")

    def _verify_tree(self):
        """Validate ranges/reachability and reject cycles without recursive traversal."""
        for no, nl, first, count in self.dirs:
            if no + nl > len(self.strings) or first + count > len(self.kids):
                raise ValueError("directory name/child range outside table")
            self.name(no, nl)
        for record in self.files:
            if record[0] + record[1] > len(self.strings):
                raise ValueError("file name outside string table")
            self.name(record[0], record[1])
        if any(child >= self.nd + self.nf for child in self.kids):
            raise ValueError("child index outside directory/file tables")
        reached_dirs, reached_files = set(), set()
        for title in self.titles:
            if title["root"] >= self.nd or any(title[part] != 0xffffffff and title[part] >= self.nd
                                                 for part in ("exefs", "romfs")):
                raise ValueError("title %d has an invalid directory index" % title["index"])
            seen, active, pending = set(), set(), [(title["root"], False)]
            while pending:
                directory, leaving = pending.pop()
                if leaving:
                    active.remove(directory)
                    seen.add(directory)
                    continue
                if directory in active:
                    raise ValueError("directory cycle in title %d" % title["index"])
                if directory in seen:
                    continue
                active.add(directory)
                pending.append((directory, True))
                _, _, first, count = self.dirs[directory]
                for child in self.kids[first:first + count]:
                    if child < self.nd:
                        pending.append((child, False))
                    else:
                        reached_files.add(child - self.nd)
            if any(title[part] != 0xffffffff and title[part] not in seen for part in ("exefs", "romfs")):
                raise ValueError("title subtree is not reachable from its root")
            reached_dirs.update(seen)
        if len(reached_dirs) != self.nd or len(reached_files) != self.nf:
            raise ValueError("orphan directory/file records are not reachable from title roots")

    def verify_walk(self, directory):
        """Already validated nodes, iteratively (including data/ and deep trees)."""
        pending = [(directory, "")]
        while pending:
            node, prefix = pending.pop()
            if node < self.nd:
                no, nl, first, count = self.dirs[node]
                for child in reversed(self.kids[first:first + count]):
                    path = prefix + self.name(*self.dirs[child][:2]) + "/" if child < self.nd else prefix
                    pending.append((child, path))
            else:
                record = self.files[node - self.nd]
                yield prefix + self.name(record[0], record[1]), record

    def name(self, off, n):
        return self.strings[off:off + n].decode("utf-8")

    def walk(self, d, prefix=""):
        """(relative path, file record) for every file under dir d."""
        no, nl, first, n = self.dirs[d]
        for k in self.kids[first:first + n]:
            if k < self.nd:
                cno, cnl = self.dirs[k][:2]
                yield from self.walk(k, prefix + self.name(cno, cnl) + "/")
            else:
                r = self.files[k - self.nd]
                yield prefix + self.name(r[0], r[1]), r

    def inner_is_zstd(self, r):
        """Whether what this frame holds is itself a zstd frame"""
        import zstandard
        self.f.seek(r[3])
        try:
            return zstandard.ZstdDecompressor().decompressobj().decompress(self.f.read(min(r[2], 1 << 20)))[:4] == ZSTD
        except zstandard.ZstdError:   # (made with one of the game's dictionaries: the game's own file)
            return False

    def is_packed(self, r, name=""):
        """A file the package keeps compressed. A frame that states no size is tried: it is the file when it unpacks to the listed size.
        A .zs file is zstd in the game too, and three of them happen to hold exactly as much as they take: that one is packed only if
        what it holds is zstd again"""
        if r[5] == 1:   # The TOC explicitly marks outer compression, even for a non-Zstd .zs payload.
            return True
        if not name:
            name = self.name(r[0], r[1])
        self.f.seek(r[3])
        head = self.f.read(18)
        if head[:4] != ZSTD:
            return False
        size = frame_content_size(head)
        if name.endswith(".zs") and (size is None or size == r[2]):
            return size == r[2] and self.inner_is_zstd(r)
        if size is not None:
            return size == r[2]
        import zstandard
        self.f.seek(r[3])
        d = zstandard.ZstdDecompressor().decompressobj()
        got = 0
        left = r[2]
        try:
            while left and got <= r[2] and not d.eof:
                b = self.f.read(min(left, 1 << 20))
                if not b:
                    break
                left -= len(b)
                got += len(d.decompress(b))
        except zstandard.ZstdError:
            return False
        return got == r[2]

    def copy_to(self, r, g, name="", *, packed=None):
        """The file, as itself, into the open file g"""
        exact = packed is not None
        if packed is None:
            packed = self.is_packed(r, name)
        self.f.seek(r[3])
        left = r[2]
        if packed:
            left = r[4] if r[5] == 1 else left + (1 << 16)   # explicit extents must not read the next member
            import zstandard
            d = zstandard.ZstdDecompressor().decompressobj()
            got = 0
            while left and not getattr(d, "eof", False):
                b = self.f.read(min(left, 8 << 20))
                if not b:
                    break
                left -= len(b)
                try:
                    out = d.decompress(b)
                except zstandard.ZstdError:   # (the end of the frame, and the next file's bytes after it)
                    break
                got += len(out)
                if got > r[2]:
                    raise SystemExit("a compressed file expanded past its declared size of %d bytes" % r[2])
                g.write(out)
            if exact and (left or d.unused_data):
                raise SystemExit("a compressed extent has trailing bytes")
            if r[5] == 1 and not d.eof:
                raise SystemExit("a compressed file ends before its frame completes")
            if got != r[2]:
                raise SystemExit("a compressed file came out as %d bytes, %d expected" % (got, r[2]))
            return
        while left:
            b = self.f.read(min(left, 8 << 20))
            if not b:
                raise SystemExit("the .nup file ends early (short read at %d of %d MB): it is incomplete. Download or copy it again"
                                 % (self.f.tell() >> 20, os.fstat(self.f.fileno()).st_size >> 20))
            g.write(b)
            left -= len(b)

    def read(self, r, n=None):
        import io
        b = io.BytesIO()
        self.copy_to(r, b)
        return b.getvalue() if n is None else b.getvalue()[:n]


class VerifySink:
    """A streamed digest and just enough prefix for the inner game-file header."""
    def __init__(self):
        self.size, self.prefix, self.hash = 0, b"", hashlib.sha256()

    def write(self, data):
        self.size += len(data)
        self.hash.update(data)
        self.prefix += data[:max(0, 18 - len(self.prefix))]


def verify(nup, titles=None, only=None):
    """Read-only container checks plus expected .zs headers, not full inner decoding/authenticity."""
    import zstandard
    bad = checked = 0
    cache = {}
    physical = os.fstat(nup.f.fileno()).st_size
    try:
        nup._verify_toc()
        nup._verify_tree()
    except (ValueError, OSError, struct.error) as error:
        print("BAD package: %s" % error)
        return dict(checked_records=0, decoded_extents=0, bad=1)
    indices = {id(record): i for i, record in enumerate(nup.files)}
    for title in nup.titles if titles is None else titles:
        parts = [(only, title[only])] if only else [("", title["root"])]
        for part, directory in parts:
            if directory == 0xffffffff:
                continue
            for name, record in nup.verify_walk(directory):
                checked += 1
                expected, flags, reserved = nup.file_details[indices[id(record)]]
                size, off, stored, codec = record[2:]
                reasons = []
                if codec not in (0, 1) or flags or any(reserved):
                    reasons.append("unsupported codec/flags/reserved bytes")
                if (size == 0 and (off or stored or codec)) or (size and (
                        not stored or off < nup.blob_offset or off % 16 or off > physical or stored > physical - off)):
                    reasons.append("invalid extent bounds")
                if codec == 0 and stored != size:
                    reasons.append("raw extent length differs from member length")
                key = (off, stored, codec, size)
                if not reasons:
                    if key not in cache:
                        sink, error = VerifySink(), None
                        try:
                            # Honor the explicit codec; leave legacy extraction heuristics unchanged.
                            nup.copy_to(record, sink, name, packed=codec == 1)
                        except (OSError, SystemExit, zstandard.ZstdError) as failure:
                            error = str(failure)
                        cache[key] = (sink.size, sink.hash.digest(), sink.prefix, error)
                    count, digest, prefix, error = cache[key]
                    if error:
                        reasons.append("member decode failed: " + error)
                    if count != size:
                        reasons.append("decoded length differs from member length")
                    if digest != expected:
                        reasons.append("member SHA-256 mismatch")
                    if name.endswith(".zs"):
                        try:
                            if prefix[:4] != ZSTD:
                                raise ValueError("missing magic")
                            zstandard.get_frame_parameters(prefix)
                        except (ValueError, zstandard.ZstdError):
                            reasons.append(".zs member lacks the expected game Zstd header")
                if reasons:
                    bad += 1
                    print("BAD title %d %016x v%d %s/%s: %s" % (
                        title["index"], title["id"], title["version"], part, name, "; ".join(reasons)))
    print("verified %d records, %d unique extents: %d bad; game .zs headers only (not inner decode/authenticity)" % (
        checked, len(cache), bad))
    return dict(checked_records=checked, decoded_extents=len(cache), bad=bad)


def pick_title(nup, want):
    if want is None:
        return max(nup.titles, key=lambda t: t["version"])   # (the newest: the update over the base)
    for t in nup.titles:
        if str(t["index"]) == want or str(t["version"]) == want or "%016x" % t["id"] == want.lower():
            return t
    raise SystemExit("no title %s" % want)


def long_path(p):
    """(Windows) p as an extended-length path (\\\\?\\...), so a game folder whose deepest file passes 260 characters (MAX_PATH)
    still unpacks, links and is read (#1468: a 262-character path in Event\\EventSetting stopped the build with
    FileNotFoundError). Python takes these everywhere, without Windows' long-paths setting. Elsewhere, or one already: as is."""
    if os.name != "nt" or not p:
        return p
    p = os.path.abspath(p)
    if p.startswith("\\\\?\\"):
        return p
    if p.startswith("\\\\"):   # (a share: \\server\share -> \\?\UNC\server\share)
        return "\\\\?\\UNC\\" + p[2:]
    return "\\\\?\\" + p


def package_parts(rel):
    parts = rel.split("/")
    if (not rel or "\\" in rel or any(not p or p in (".", "..") or p[-1] in " ." or
            any(c in '<>:"|?*' or ord(c) < 32 for c in p) or p.split(".")[0].rstrip(" ").upper() in WINDOWS_DEVICES
            for p in parts)):
        raise SystemExit("unsafe path in the package: %r" % rel)
    return parts


def is_link(info):
    return stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)


class Destination:
    """extraction_path over a whole extraction: the output resolved once, each folder checked or made once"""
    def __init__(self, out):
        self.root = long_path(os.path.realpath(os.path.abspath(out)))
        self.folders = set()

    def folder(self, path):
        if path not in self.folders:
            try:
                info = os.lstat(path)
            except FileNotFoundError:
                os.makedirs(path, exist_ok=True)
            else:
                if is_link(info):
                    raise SystemExit("extraction path is a link or reparse point: %s" % path)
            self.folders.add(path)

    def path(self, part, rel):
        parts = package_parts(rel)
        at = self.root
        self.folder(at)
        for name in (part, *parts[:-1]):
            at = os.path.join(at, name)
            self.folder(at)
        return os.path.join(at, parts[-1])


def extraction_path(out, part, rel, temporary_suffix=".part"):
    """A package path stays under its part, without following existing output links."""
    parts = package_parts(rel)
    root = long_path(os.path.realpath(os.path.abspath(out)))
    base = os.path.join(root, part)
    dst = os.path.join(base, *parts)
    if os.path.commonpath((os.path.normcase(base), os.path.normcase(os.path.abspath(dst)))) != os.path.normcase(base):
        raise SystemExit("path leaves the extraction folder: %r" % rel)
    at = root
    paths = [at]
    for p in (part, *parts):
        at = os.path.join(at, p)
        paths.append(at)
    temporary = dst + temporary_suffix
    paths.append(temporary)
    for path in paths:
        try:
            info = os.lstat(path)
        except FileNotFoundError:
            continue
        if is_link(info):
            raise SystemExit("extraction path is a link or reparse point: %s" % path)
        if path == temporary and info.st_nlink > 1:
            raise SystemExit("extraction temporary file has other hard links: %s" % path)
    return dst


class ProgressSink:
    """Report successful writes without changing the package reader's copy interface."""
    def __init__(self, output, report):
        self.output = output
        self.report = report

    def write(self, data):
        count = self.output.write(data)
        self.report(count)
        return count


class HeadSink:
    """Check the Zstd prefix and available member digest during extraction, without reopening the file."""
    def __init__(self, output, expected):
        self.output = output
        self.expected = expected
        self.digest = hashlib.sha256() if expected is not None else None
        self.head = b""

    def write(self, data):
        if len(self.head) < 4:
            self.head += bytes(data[:4 - len(self.head)])
        if self.digest is not None:
            self.digest.update(data)
        return self.output.write(data)

    def damaged(self):
        return not zstd_start(self.head) or (self.digest is not None and self.digest.digest() != self.expected)

# Damaged romfs .zs files found while extracting, beside
# the extracted romfs: its first line HEADS_MAGIC, then one path under romfs a line, with "/"
HEADS_NAME = "romfs_zstd_heads.txt"
HEADS_MAGIC = "hookshot romfs zstd heads 1"


def file_system(path):
    """The file system of the volume path is on (NTFS, exFAT, FAT32, ReFS): Windows only, None elsewhere"""
    if os.name != "nt":
        return None
    import ctypes
    k = ctypes.WinDLL("kernel32")
    root, name = ctypes.create_unicode_buffer(261), ctypes.create_unicode_buffer(261)
    if not k.GetVolumePathNameW(path, root, 261) or not k.GetVolumeInformationW(root, None, 0, None, None, None, name, 261):
        return None
    return name.value


def out_of_space(error, dst, part, rel, size):
    """A full drive said with the member being written and what its drive reports, to tell a full disk from a write the drive
    refused with room to spare (issue 1388: 196 GB free on NTFS, stopped at 12 GB). None for any other error"""
    if not isinstance(error, OSError) or error.errno != errno.ENOSPC:
        return None
    folder = os.path.dirname(dst)
    try:
        free = "%.1f GB free there" % (shutil.disk_usage(folder).free / 1e9)
    except OSError:
        free = "free space unknown"
    kind = file_system(folder)
    return OSError(errno.ENOSPC, "No space left on device writing %s/%s (%d bytes): %s%s" % (
        part, rel, size, free, ", " + kind if kind else ""))


def extract(nup, t, out, only=None, quiet=False):
    total = n = written = 0
    digests = {}
    details = getattr(nup, "file_details", ())
    if only != "exefs" and details:
        digests = {id(record): expected for record, (expected, _, _) in zip(nup.files, details)
                   if expected != bytes(32)}
    interval = 2
    next_report = 0 if quiet else monotonic() + interval

    def progress(size):
        nonlocal written, next_report
        written += size
        now = monotonic()
        if now >= next_report:
            print("  %.1f MiB written (%d files completed)" % (written / (1 << 20), n), flush=True)
            next_report = now + interval

    where = Destination(out)
    stale = True
    for part in ("exefs", "romfs"):
        if only and part != only:
            continue
        damaged = [] if part == "romfs" else None
        for rel, r in nup.walk(t[part]):
            dst = where.path(part, rel)
            checked = damaged is not None and rel.endswith(".zs")
            expected = digests.get(id(r)) if checked else None
            try:
                info = os.lstat(dst)
            except FileNotFoundError:
                info = None
            if info is not None and is_link(info):
                raise SystemExit("extraction path is a link or reparse point: %s" % dst)
            # (an interrupted member is shorter than its size: written again)
            if info is not None and stat.S_ISREG(info.st_mode) and info.st_size == r[2]:
                if checked:
                    with open(dst, "rb") as f:
                        prefix = f.read(4)
                        if not zstd_start(prefix):
                            damaged.append(rel)
                        elif expected is not None:
                            digest = hashlib.sha256(prefix)
                            for block in iter(lambda: f.read(1 << 20), b""):
                                digest.update(block)
                            if digest.digest() != expected:
                                damaged.append(rel)
                continue
            # Written in place: a temporary and a rename per member took most of an extraction's time on Windows.
            # A stale file is unlinked, not truncated, so another hard link to it keeps its data.
            if info is not None:
                os.unlink(dst)
            if stale:   # (an interrupted builder that wrote through a temporary left it beside the first member still to write)
                stale = False
                with suppress(OSError):
                    os.unlink(dst + ".part")
            g = open(os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)), "wb")
            try:
                with g:
                    sink = HeadSink(g, expected) if checked else g
                    nup.copy_to(r, sink if quiet else ProgressSink(sink, progress), rel)
                if checked and sink.damaged():
                    damaged.append(rel)
            except BaseException as error:
                full = out_of_space(error, dst, part, rel, r[2])   # (measured before the member's space is freed)
                # Free the failed member's space without hiding the extraction error.
                with suppress(OSError):
                    os.unlink(dst)
                if full:
                    raise full from error
                raise
            total += r[2]
            n += 1
        if damaged is not None:   # (the whole romfs was walked: its answer is complete)
            heads = os.path.join(out, HEADS_NAME)
            with open(heads + ".part", "w", encoding="utf-8", newline="\n") as f:
                f.write(HEADS_MAGIC + "\n" + "".join(rel + "\n" for rel in damaged))
            os.replace(heads + ".part", heads)
    return n, total


def read_heads(path):
    """HEADS_NAME's list (paths under romfs, "/"), or None when the file is missing or not one of these"""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().split("\n")
    except OSError:
        return None
    if not lines or lines[0] != HEADS_MAGIC:
        return None
    return read_heads_text("\n".join(lines[1:]))


def read_heads_text(text):
    """The paths of HEADS_NAME's list after its first line"""
    return [line for line in text.split("\n") if line]


def heal(nup, t, out, names):
    """An update's exefs need not hold every module whole: what is not an NSO there is taken from the package's other titles"""
    for m in names:
        p = extraction_path(out, "exefs", m)
        have = b""
        if os.path.exists(p):
            with open(p, "rb") as f:
                have = f.read(16)
        if have[:4] == b"NSO0":
            continue
        for other in reversed(nup.titles):
            if other["index"] == t["index"]:
                continue
            for rel, r in nup.walk(other["exefs"]):
                if rel == m and nup.read(r, 4) == b"NSO0":
                    with open(p + ".part", "wb") as g:
                        g.write(nup.read(r))
                    os.replace(p + ".part", p)
                    print("  %s: taken from title %d (v%d)" % (m, other["index"], other["version"]))
                    break
            else:
                continue
            break
        else:
            if have:
                print("  %s is not a game module in any title: it starts %s" % (m, have.hex()))


def zstd_start(head):
    """Whether a file starts as zstd data: a frame, or a skippable frame (magic 0x184D2A50 to 0x184D2A5F)"""
    return head == ZSTD or (len(head) == 4 and head[1:] == b"\x2a\x4d\x18" and head[0] & 0xf0 == 0x50)


def still_in_nup_frame(f, head):
    """Whether a .zs file that starts as zstd (head: its first 18 bytes) is the .nup's own frame round a damaged file, as
    builders before #456 wrote them (#2361): a frame whose content is as large as the file, and that starts with zeros or
    does not decompress. Only those are decompressed, and only their start: a game .zs file's content is larger than the
    file, and most of them need the game's dictionary to decompress at all"""
    if frame_content_size(head) != os.fstat(f.fileno()).st_size:
        return False
    import zstandard
    f.seek(0)
    try:
        return not any(zstandard.ZstdDecompressor().stream_reader(f, read_size=1 << 14).read(16))
    except zstandard.ZstdError:
        return True


def damaged_game_files(romfs, progress=None):
    """The romfs .zs files that do not start as zstd: in the 23.3 GB dumps three of them begin with zeros (the complete
    dump is about 23.8 GB), and the game then leaves out every model that uses one: the Water Temple's walls and floors,
    the Great Fairy Fountain's floor (#1002, #718, #1259). Also those still in the .nup's zstd frame (still_in_nup_frame).
    progress(done, total), when given, after each file: the files are listed first, then opened one by one (about
    100,000, minutes where a virus scanner looks at each one)"""
    root = long_path(romfs)   # (walked and read as long paths; the paths returned are under romfs as given)
    paths = []   # (the long path read, the path under romfs as given)
    for folder, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(folder, root)
        paths += [(os.path.join(folder, name), os.path.normpath(os.path.join(romfs, rel, name)))
                  for name in sorted(files) if name.endswith(".zs")]
    found = []
    for i, (path, given) in enumerate(paths):
        with open(path, "rb") as f:
            head = f.read(18)
            if not zstd_start(head[:4]) or still_in_nup_frame(f, head):
                found.append(given)
        if progress:
            progress(i + 1, len(paths))
    return found


def stand_in_for(path, damaged=()):
    """A whole file of the same kind beside a damaged one, to load in its place: the same last part of the name
    (_Nrm.bntx.zs: a normal map for a normal map), the longest common start of the name, then the nearest size. Never
    one of the damaged files, even one already given a stand-in"""
    folder, name = os.path.split(path)
    suffix = re.search(r"(_[A-Za-z]+\d*)?(\.[^_]*)$", name).group(0)
    size = os.path.getsize(long_path(path))
    best = None
    for other in os.listdir(long_path(folder)):
        if other == name or not other.endswith(suffix):
            continue
        candidate = os.path.join(folder, other)
        if candidate in damaged:
            continue
        with open(long_path(candidate), "rb") as f:
            if not zstd_start(f.read(4)):
                continue
        score = (len(os.path.commonprefix((name, other))), -abs(os.path.getsize(long_path(candidate)) - size))
        if best is None or score > best[0]:
            best = (score, candidate)
    return best and best[1]


def stand_in_damaged(romfs, progress=None, damaged=None):
    """Each damaged .zs file in an unpacked romfs replaced by a copy of its stand-in: (damaged, stand-in or None) each.
    Only for a romfs this tool unpacked: the player's own folder is never written to. progress: damaged_game_files'.
    damaged: the paths already known (the extraction's HEADS_NAME), so no file is opened to find them"""
    done = []
    if damaged is None:
        damaged = damaged_game_files(romfs, progress)
    excluded = set(damaged)
    for path in damaged:
        other = stand_in_for(path, excluded)
        if other:
            shutil.copyfile(long_path(other), long_path(path + ".part"))
            os.replace(long_path(path + ".part"), long_path(path))
        done.append((path, other))
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("nup")
    ap.add_argument("--extract", metavar="DIR")
    ap.add_argument("--verify", action="store_true", help="read-only hashes/extents and expected game .zs headers; all titles unless --title")
    ap.add_argument("--title", help="index, version or title id (extract default: newest; verify default: all)")
    ap.add_argument("--only", choices=("exefs", "romfs"))
    a = ap.parse_args()
    if a.verify and a.extract:
        ap.error("--verify does not extract; run extraction separately")
    try:
        nup = Nup(a.nup, strict=a.verify)
    except (OSError, ValueError, struct.error, IndexError, SystemExit) as error:
        if not a.verify:
            raise
        print("BAD package: %s" % error)
        return 1
    if a.verify:
        try:
            titles = None if a.title is None else [pick_title(nup, a.title)]
            return 1 if verify(nup, titles, a.only)["bad"] else 0
        finally:
            nup.f.close()
    print("%s: %.2f GB, %d titles, %d dirs, %d files" % (os.path.basename(a.nup), nup.file_size / 1e9,
                                                          len(nup.titles), nup.nd, nup.nf))
    for t in nup.titles:
        ex = list(nup.walk(t["exefs"]))
        ro = list(nup.walk(t["romfs"]))
        print("title %d: %016x v%d  exefs %d files (%s)  romfs %d files, %.2f GB" % (
            t["index"], t["id"], t["version"], len(ex), ", ".join("%s %d" % (p, r[2]) for p, r in ex),
            len(ro), sum(r[2] for _, r in ro) / 1e9))
        same = sum(1 for _, r in ro if r[2] != r[4])
        if same:
            print("  (%d romfs files with two different sizes)" % same)
    if a.extract:
        t = pick_title(nup, a.title)
        # Measure the destination volume, including an existing mount or junction.
        folder = os.path.realpath(a.extract)
        while not os.path.isdir(folder):
            parent = os.path.dirname(folder)
            if parent == folder:
                break
            folder = parent
        free = shutil.disk_usage(folder).free
        print("extracting title %d (v%d) to %s (%.0f GB free)" % (t["index"], t["version"], a.extract, free / 1e9))
        n, total = extract(nup, t, a.extract, a.only)
        print("done: %d files written, %.2f GB" % (n, total / 1e9))
        if a.only != "romfs":   # (main is ZBIC-compressed on firmware 22+: made a plain NSO for the tools)
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import unpack
            heal(nup, t, a.extract, unpack.NAMES)
            for m in unpack.NAMES:
                p = extraction_path(a.extract, "exefs", m, temporary_suffix=".plain")
                if os.path.exists(p) and unpack.unpack_nso(p):
                    print("  %s: ZBIC -> plain NSO" % m)


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):   # (-I ignores PYTHONIOENCODING: a .nup's or a file's name in other letters crashed a print)
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    sys.exit(main())
