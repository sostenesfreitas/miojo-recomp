"""Synthetic actual-reader verification: python -I -B nx2/test_nup_verify.py.

No game bytes, archive extraction or compiler. Requires existing zstandard dependency.
"""
import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zstandard as zs

RUN_ALL_MODULES = ["zstandard"]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
a = p.parse_args()
spec = importlib.util.spec_from_file_location('tested_nup', a.source / 'nx2/nup.py')
nup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nup)
compressor = zs.ZstdCompressor(write_checksum=True)


def package(rows, titles=1):
    """(title index, name, logical bytes, codec); identical contents share extents."""
    files_at = 0xb0 + 64 * titles + 24 * titles
    strings = b''.join(name.encode() for _, name, _, _ in rows)
    strings_at = files_at + 80 * len(rows) + 4 * len(rows)
    blob_at = (strings_at + len(strings) + 15) & ~15
    blob = bytearray(blob_at)
    blob[:4], blob[0x80:0x84] = b'NUP\x01', b'TOC\x01'
    struct.pack_into('<HHQQ', blob, 4, 0x80, 1, 0x80, strings_at + len(strings) - 0x80)
    struct.pack_into('<IQQQQ', blob, 0x84, titles, titles, len(rows), strings_at - 0x80, len(strings))
    struct.pack_into('<Q', blob, 0x38, blob_at)
    blob[strings_at:strings_at + len(strings)] = strings
    slot = 0
    for t in range(titles):
        struct.pack_into('<QIIII', blob, 0xb0 + 64 * t, 0x1000 + t, t, 0xffffffff, t, t + 1)
        members = [i for i, row in enumerate(rows) if row[0] == t]
        struct.pack_into('<4I', blob, 0xb0 + 64 * titles + 24 * t, 0, 0, slot, len(members))
        for i in members:
            struct.pack_into('<I', blob, files_at + 80 * len(rows) + 4 * slot, titles + i)
            slot += 1
    name_at, seen = 0, {}
    for i, (_, name, logical, codec) in enumerate(rows):
        stored = compressor.compress(logical) if codec else logical
        key = (stored, codec)
        if key not in seen:
            blob.extend(bytes((-len(blob)) % 16))
            seen[key] = len(blob)
            blob.extend(stored)
        at = files_at + 80 * i
        struct.pack_into('<IIQQQ', blob, at, name_at, len(name.encode()), len(logical), seen[key], len(stored))
        blob[at + 32:at + 64] = hashlib.sha256(logical).digest()
        blob[at + 64] = codec
        name_at += len(name.encode())
    struct.pack_into('<Q', blob, 0x40, len(blob))
    return blob, files_at


def seal(blob):
    offset, size = struct.unpack_from('<QQ', blob, 8)
    blob[24:56] = hashlib.sha256(blob[offset:offset + size]).digest()


with tempfile.TemporaryDirectory(prefix='nup-verify-') as temp:
    path = Path(temp) / 'synthetic.nup'

    def check(blob, bad, titles=None):
        seal(blob)
        path.write_bytes(blob)
        reader = nup.Nup(str(path))
        calls, copy = [], reader.copy_to
        def counted(*args, **kwargs):
            calls.append(args[0])
            return copy(*args, **kwargs)
        reader.copy_to = counted
        try:
            with redirect_stdout(io.StringIO()) as output:
                result = nup.verify(reader, titles)
            assert result['bad'] == bad, output.getvalue()
            return result, len(calls), output.getvalue()
        finally:
            reader.f.close()

    game = compressor.compress(b'self-authored game payload' * 500)
    good, at = package([(0, 'raw.bin', b'raw bytes', 0), (0, 'raw.zs', game, 0), (0, 'outer.zs', game, 1)])
    assert check(good, 0)[0]['checked_records'] == 3
    empty, at = package([(0, 'empty.bin', b'', 0)])
    struct.pack_into('<QQ', empty, at + 16, 0, 0)  # canonical empty extent
    check(empty, 0)
    # Hash-valid zero prefix: container integrity alone must not bless the game asset.
    damaged, at = package([(0, 'bad.zs', bytes(12288) + b'not a Zstd frame', 1)])
    assert 'expected game Zstd header' in check(damaged, 1)[2]
    dedup, at = package([(0, 'one.bin', b'shared bytes', 0), (0, 'two.bin', b'shared bytes', 0)])
    dedup[at + 80 + 32] ^= 1
    result, calls, output = check(dedup, 1)
    assert calls == result['decoded_extents'] == 1 and 'SHA-256 mismatch' in output
    for problem in ('checksum', 'truncated', 'trailing', 'bounds', 'codec', 'flags', 'reserved'):
        blob, at = package([(0, 'test.zs', game, 1)])
        off, size = struct.unpack_from('<QQ', blob, at + 16)
        if problem == 'checksum': blob[off + size - 1] ^= 1
        elif problem == 'truncated': struct.pack_into('<Q', blob, at + 24, size - 1)
        elif problem == 'trailing':
            blob.extend(b'x')
            struct.pack_into('<Q', blob, 0x40, len(blob))
            struct.pack_into('<Q', blob, at + 24, size + 1)
        elif problem == 'bounds': struct.pack_into('<Q', blob, at + 16, (len(blob) + 15) & ~15)
        else: blob[at + {'codec': 64, 'flags': 65, 'reserved': 66}[problem]] = 9
        check(blob, 1)
    blob, at = package([(0, 'bad.zs', bytes(32), 0), (1, 'good.zs', game, 0)], titles=2)
    check(blob, 1)
    selected = dict(index=1, id=0x1001, version=2, root=1, exefs=0xffffffff, romfs=1)
    check(blob, 0, [selected])
    before = path.read_bytes()
    cli = [sys.executable, '-I', '-B', str(a.source / 'nx2/nup.py'), str(path), '--verify', '--only', 'romfs']
    assert subprocess.run(cli, capture_output=True).returncode == 1
    assert subprocess.run(cli + ['--title', '0000000000001001'], capture_output=True).returncode == 0
    assert path.read_bytes() == before
    # Rehashed short TOC must not leave parsed tables outside the hashed region.
    def cli_bad(blob, reason, rehash=True):
        if rehash:
            seal(blob)
        path.write_bytes(blob)
        before = path.read_bytes()
        result = subprocess.run(cli, capture_output=True, text=True, encoding='utf-8', timeout=5)
        assert result.returncode == 1 and 'BAD package:' in result.stdout, (result.stdout, result.stderr)
        assert reason in result.stdout and 'Traceback' not in result.stderr, (reason, result.stdout, result.stderr)
        assert path.read_bytes() == before

    for problem, reason in (
            ('short-toc', 'exceed declared TOC'), ('strings', 'exceed declared TOC'),
            ('child', 'child index'), ('directory-range', 'child range'),
            ('title', 'invalid directory index'), ('cycle', 'directory cycle'),
            ('orphan', 'orphan'), ('utf8', 'utf-8')):
        blob, at = package([(0, 'test.zs', game, 0)])
        dirs_at, kids_at = 0xb0 + 64, at + 80
        if problem == 'short-toc': struct.pack_into('<Q', blob, 0x10, 0x30)
        elif problem == 'strings': struct.pack_into('<Q', blob, 0xa0, 1 << 20)
        elif problem == 'child': struct.pack_into('<I', blob, kids_at, 99)
        elif problem == 'directory-range': struct.pack_into('<I', blob, dirs_at + 8, 1)
        elif problem == 'title': struct.pack_into('<I', blob, 0xb0 + 8, 1)
        elif problem == 'cycle': struct.pack_into('<I', blob, kids_at, 0)
        elif problem == 'orphan': struct.pack_into('<I', blob, dirs_at + 12, 0)
        elif problem == 'utf8': blob[kids_at + 4] = 0xff
        cli_bad(blob, reason)
    blob, at = package([(0, 'one.bin', b'one', 0), (1, 'two.bin', b'two', 0)], titles=2)
    struct.pack_into('<I', blob, 0xb0 + 16, 1)
    cli_bad(blob, 'subtree is not reachable')
    struct.pack_into('<I', blob, 0xb0 + 16, 0)
    struct.pack_into('<I', blob, 0xb0 + 64 + 8, 0)
    struct.pack_into('<I', blob, 0xb0 + 64 + 16, 0)
    cli_bad(blob, 'orphan')
    blob, at = package([(0, 'test.zs', game, 0)])
    seal(blob)
    struct.pack_into('<I', blob, at + 80, 2)  # invalid child AND unhashed TOC
    cli_bad(blob, 'TOC SHA-256 mismatch', rehash=False)
    # A prefix-valid but corrupt near match must not be selected as a stand-in.
    destination, near, whole = ('Wall_A_Nrm.bntx.zs', 'Wall_B_Nrm.bntx.zs', 'Other_Nrm.bntx.zs')
    blob, at = package([(0, destination, bytes(64), 0), (0, near, game, 0), (0, whole, game, 1)])
    offset, size = struct.unpack_from('<QQ', blob, at + 80 + 16)
    blob[offset + size - 1] ^= 1
    seal(blob)
    path.write_bytes(blob)
    reader = nup.Nup(str(path))
    output = Path(temp) / 'stand-in'
    try:
        for count in (3, 0):
            with redirect_stdout(io.StringIO()):
                assert nup.extract(reader, reader.titles[0], str(output), only='romfs', quiet=True)[0] == count
            assert nup.read_heads(output / nup.HEADS_NAME) == [destination, near]
        romfs = output / 'romfs'
        bad = [str(romfs / member) for member in (destination, near)]
        assert dict(nup.stand_in_damaged(romfs, damaged=bad)) == {
            member: str(romfs / whole) for member in bad
        }
        assert all(Path(member).read_bytes() == game for member in bad)
        assert (romfs / whole).read_bytes() == game and path.read_bytes() == blob
    finally:
        reader.f.close()

    assert 'COM\u00b9' in nup.WINDOWS_DEVICES and 'LPT\u00b3' in nup.WINDOWS_DEVICES
    try:
        nup.extraction_path(temp, 'romfs', 'COM\u00b9/file')
    except SystemExit:
        pass
    else:
        raise AssertionError('base superscript Windows device protection changed')
print('PASS: raw/codec1, dedup hashes, hash-valid damaged .zs, checksum/extent/metadata failures, title scope, strict TOC/tree rejection and read-only CLI')
