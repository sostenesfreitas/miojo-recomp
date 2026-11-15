"""Game-free: a read of the .nup that the system refuses partway through (issue 295) stops nup.py with what that means and
what to do, not a bare "PermissionError: [Errno 13] Permission denied". On Windows, a real lock held by another process.
python -I -B nx2/test_nup_read_error.py
"""
import argparse
import errno
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
a = p.parse_args()
spec = importlib.util.spec_from_file_location('tested_nup', a.source/'nx2/nup.py')
nup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nup)

DATA = 3 << 20   # (a member long enough that the read is not served from the file object's buffer)


def package(name, data):
    """One title, one root directory, one stored (uncompressed) file; no game content"""
    name = name.encode()
    file_at = 0x80 + 0x30 + 0x40 + 24
    strings_at = file_at + 80 + 4
    data_at = (strings_at + len(name) + 15) & ~15
    blob = bytearray(data_at + len(data))
    blob[:4] = b'NUP\x01'
    struct.pack_into('<Q', blob, 0x40, len(blob))
    blob[0x80:0x84] = b'TOC\x01'
    struct.pack_into('<IQQQQ', blob, 0x84, 1, 1, 1, strings_at-0x80, len(name))
    struct.pack_into('<QIIII', blob, 0xb0, 1, 0, 0, 0, 1)
    struct.pack_into('<4I', blob, file_at-24, 0, 0, 0, 1)
    struct.pack_into('<IIQQQ', blob, file_at, 0, len(name), len(data), data_at, len(data))
    struct.pack_into('<I', blob, file_at+80, 1)
    blob[strings_at:strings_at+len(name)] = name
    blob[data_at:] = data
    return blob, data_at


class Refusing:
    """The package's file object, its reads refused with error"""
    def __init__(self, f, error):
        self.f, self.error = f, error

    def read(self, n=-1):
        raise self.error

    def __getattr__(self, name):
        return getattr(self.f, name)


def stopped(reader):
    relative, record = next(reader.walk(0))
    try:
        reader.copy_to(record, io.BytesIO(), relative)
    except SystemExit as e:
        return str(e.code)
    raise AssertionError('a refused read was not reported')


with tempfile.TemporaryDirectory(prefix='nup-read-error-') as temporary:
    path = os.path.join(temporary, 'game.nup')
    blob, data_at = package('member.bin', bytes(range(256)) * (DATA // 256))
    Path(path).write_bytes(blob)

    reader = nup.Nup(path)
    with reader.f:
        relative, record = next(reader.walk(0))
        out = io.BytesIO()
        reader.copy_to(record, out, relative)
        assert out.getvalue() == blob[data_at:], 'an ordinary read changed'

        reader.f = Refusing(reader.f, OSError(errno.EIO, 'Input/output error'))
        said = stopped(reader)
        assert said.startswith('Your drive could not read game.nup at 0 of 3 MB: the drive reported an I/O error'), said
        assert temporary not in said, 'the message names the file, not its folder: ' + said

        reader.f = Refusing(reader.f.f, PermissionError(errno.EACCES, 'Permission denied'))
        for code, start in ((33, 'Windows would not let the build read game.nup at 0 of 3 MB: another program has that part '
                                 'of the file locked (Windows error 33)'),
                            (23, 'Your drive could not read game.nup at 0 of 3 MB: a read failed its data check (Windows error 23)'),
                            (5, 'The build could not read game.nup at 0 of 3 MB (Windows error 5)'),
                            (None, 'The build could not read game.nup at 0 of 3 MB (Permission denied)')):
            real, nup.windows_error = nup.windows_error, lambda error, code=code: code
            try:
                said = stopped(reader)
            finally:
                nup.windows_error = real
            assert said.startswith(start), (code, said)

    if os.name == 'nt':
        # Another process locks the member's last MB, as a program that holds the file can: the read that reaches it is
        # refused with ERROR_LOCK_VIOLATION, which Python's read() gives as errno 13
        holder = subprocess.Popen([sys.executable, '-I', '-c', r'''
import ctypes, ctypes.wintypes as w, msvcrt, sys
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
class Overlapped(ctypes.Structure):
    _fields_ = [('Internal', ctypes.c_void_p), ('InternalHigh', ctypes.c_void_p), ('Offset', w.DWORD),
                ('OffsetHigh', w.DWORD), ('hEvent', w.HANDLE)]
f = open(sys.argv[1], 'rb')
at = Overlapped(); at.Offset = int(sys.argv[2])
ok = k32.LockFileEx(w.HANDLE(msvcrt.get_osfhandle(f.fileno())), 2, 0, 1 << 20, 0, ctypes.byref(at))
print('locked' if ok else 'not locked %d' % ctypes.get_last_error(), flush=True)
sys.stdin.read()
''', path, str(data_at + DATA - (1 << 20))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            creationflags=0x08000000)   # (CREATE_NO_WINDOW)
        try:
            assert holder.stdout.readline().strip() == 'locked'
            reader = nup.Nup(path)
            with reader.f:
                said = stopped(reader)
            assert said.startswith('Windows would not let the build read game.nup at 0 of 3 MB: another program has that '
                                   'part of the file locked (Windows error 33)'), said
        finally:
            holder.stdin.close()
            holder.wait(timeout=30)

print('nup read errors: ok')
