"""Actual Nup read/copy classification with synthetic bytes; requires existing zstandard.
python -I -B nx2/test_nup_read_name.py
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import importlib.util
import io
from pathlib import Path
import zstandard

RUN_ALL_MODULES = ["zstandard"]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('nup_read_name', args.source / 'nx2/nup.py')
nup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nup)

# Synthetic frame, not a game asset. Its compressed and decoded lengths are both 49.
FRAME = bytes.fromhex('28b52ffd20314501001402000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f00010041c013')
PAYLOAD = bytes(range(32)) + bytes(17)
assert len(FRAME) == len(PAYLOAD) == 49
assert zstandard.ZstdDecompressor().decompress(FRAME) == PAYLOAD


def member(data, name, size=None, codec=0):
    reader = nup.Nup.__new__(nup.Nup)
    reader.strings = name.encode('utf-8')
    reader.f = io.BytesIO(bytes(32) + data)
    record = (0, len(reader.strings), len(data) if size is None else size, 32, len(data), codec)
    reader.files = [record]
    return reader, record


def check(label, data, name, expected, size=None, codec=0):
    reader, record = member(data, name, size, codec)
    assert reader.read(record, 18) == expected[:18], label + ': read lost filename context'
    assert reader.read(record) == expected, label + ': full read'
    for supplied in (None, name):
        output = io.BytesIO()
        if supplied is None:
            reader.copy_to(record, output)
        else:
            reader.copy_to(record, output, supplied)
        assert output.getvalue() == expected, label + ': copy/name parity'
    print('PASS:', label)
    return reader, record


reader, record = check('raw equal-length .zs', FRAME, 'x.bcul.zs', FRAME)
assert not reader.is_packed(record)
assert reader.is_packed(record, 'main'), 'provided filename must remain authoritative'
plain = b'not another Zstd frame' * 3
compress = zstandard.ZstdCompressor().compress
check('explicit outer codec1', compress(plain), 'x.bcul.zs', plain, len(plain), 1)
check('legacy exefs outer frame', FRAME, 'main', PAYLOAD, len(PAYLOAD))
check('ordinary raw exefs', b'NSO0' + bytes(40), 'main', b'NSO0' + bytes(40))
check('legacy nested outer .zs', compress(FRAME), 'x.bcul.zs', FRAME, len(FRAME))
