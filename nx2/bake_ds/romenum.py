"""Local bake, step 1: every shader program in a game tree, as hookshot_shaders.bin (bake-deepseek).

    python3 nx2/bake_ds/romenum.py <romfs> [--out hookshot_shaders.bin] [--manifest enum.json]

The walk and the containers are the ones nx2/shader_census.py documents (BNSH variations, agl's BAHS,
NDSR networks, NDVE upscaler tables, Effect's .vsetb BNSH, SARC wrappers, zstd with the game's
dictionaries). Uses the build's zstandard package or standalone system libzstd; no game files in the repository.
The only thing it shares with the census is the format, not code or files.

Output: the same HKSEED01 the back end reads (gpu/vk/vk_shader.cpp:1844 seed_open):

    "HKSEED01", u32 programs, u32 stages;
    each stage once: u64 key, u32 stage, u32 control bytes, u32 code bytes, control, code
    then each program: u32 count, u32 stage indices

The stage key is the runtime's own: FNV-1a 64 over control[0,0xd8) then code[0,end) (vk_shader.cpp:135
code_key), so the seed's dedupe key, the manifest ID and the bank's filename are one identity. The census
writes sha1(control+code)[:8] there instead; seed_open recomputes FNV and ignores it (vk_shader.cpp:1872),
so either reads, but FNV dedupes exactly what the runtime treats as one shader. (Bake-Mimo made the same
point for its stable IDs.)

zstd: the Python package or libzstd through ctypes (frames with and without a dictionary), because the game's archives are
.zs and the dictionaries are player data. A plain zstd CLI fallback exists; nothing is written outside
--out/--manifest.
"""
import argparse
import collections
import ctypes
import ctypes.util
import json
import os
import re
import struct
import subprocess
import sys

CONTROL_MAGIC = b"\x34\x12\x76\x98"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
M64 = (1 << 64) - 1
SLOTS = (8, 16, 24, 32, 40, 48, 56, 64, 72)
FOLDERS = ("Shader", "Layout2Shader", "Lib", "Movie", "Effect")


# ---------------------------------------------------------------- zstd (ctypes)
class _Zstd:
    UNKNOWN = (1 << 64) - 1
    ERROR = (1 << 64) - 2

    def __init__(self):
        name = ctypes.util.find_library("zstd") or "libzstd.so.1"
        self.lib = ctypes.CDLL(name)
        L = self.lib
        L.ZSTD_createDCtx.restype = ctypes.c_void_p
        L.ZSTD_freeDCtx.argtypes = [ctypes.c_void_p]
        L.ZSTD_decompressDCtx.restype = ctypes.c_size_t
        L.ZSTD_decompressDCtx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                          ctypes.c_void_p, ctypes.c_size_t]
        L.ZSTD_decompress_usingDict.restype = ctypes.c_size_t
        L.ZSTD_decompress_usingDict.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                                ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                                ctypes.c_size_t]
        L.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
        L.ZSTD_getFrameContentSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        L.ZSTD_isError.restype = ctypes.c_uint
        L.ZSTD_isError.argtypes = [ctypes.c_size_t]
        L.ZSTD_getErrorName.restype = ctypes.c_char_p
        L.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]

    def _err(self, code):
        return self.lib.ZSTD_getErrorName(code).decode("ascii", "replace")

    def decompress(self, data, dictionaries=()):
        """A frame, or the data itself when it is not one. Dictionaries are tried in order, as the census does."""
        if data[:4] != ZSTD_MAGIC:
            return data
        src = ctypes.create_string_buffer(data, len(data))
        size = self.lib.ZSTD_getFrameContentSize(src, len(data))
        cap = size if size not in (self.UNKNOWN, self.ERROR) else max(1 << 20, len(data) * 64)
        attempts = [None, *dictionaries]
        last = None
        while True:
            dst = ctypes.create_string_buffer(cap)
            for d in attempts:
                ctx = self.lib.ZSTD_createDCtx()
                try:
                    if d is None:
                        n = self.lib.ZSTD_decompressDCtx(ctx, dst, cap, src, len(data))
                    else:
                        dbuf = ctypes.create_string_buffer(d, len(d))
                        n = self.lib.ZSTD_decompress_usingDict(ctx, dst, cap, src, len(data), dbuf, len(d))
                finally:
                    self.lib.ZSTD_freeDCtx(ctx)
                if not self.lib.ZSTD_isError(n):
                    return dst.raw[:n]
                last = self._err(n)
            if size not in (self.UNKNOWN, self.ERROR):
                raise ValueError("zstd: %s" % last)
            if cap >= (1 << 31):
                raise ValueError("zstd: %s" % last)
            cap *= 4   # (a frame with no content size: grow, as a stream would)


_ZSTD = None


class _PythonZstd:
    """Use the pinned build dependency on platforms without a discoverable libzstd."""
    def __init__(self):
        import zstandard
        self.zstd = zstandard

    def decompress(self, data, dictionaries=()):
        if data[:4] != ZSTD_MAGIC:
            return data
        last = None
        for dictionary in (None, *dictionaries):
            try:
                d = self.zstd.ZstdCompressionDict(dictionary) if dictionary is not None else None
                decoder = self.zstd.ZstdDecompressor(dict_data=d)
                cap = 0
                if self.zstd.frame_content_size(data) < 0:
                    # max_output_size reserves the whole limit, even for a tiny
                    # unknown-size frame. Count in bounded chunks first, then
                    # validate the complete frame with an exactly sized buffer.
                    with decoder.stream_reader(data, read_across_frames=False) as stream:
                        while chunk := stream.read(1 << 20):
                            cap += len(chunk)
                            if cap > 1 << 31:
                                raise ValueError("zstd: frame exceeds output bound")
                    cap = max(1, cap)
                return decoder.decompress(data, max_output_size=cap, allow_extra_data=False)
            except self.zstd.ZstdError as error:
                last = error
        raise ValueError("zstd: %s" % last)


def zstd():
    global _ZSTD
    if _ZSTD is None:
        try:
            _ZSTD = _PythonZstd()
        except ImportError:
            _ZSTD = _Zstd()   # (standalone enumeration still works with system libzstd)
    return _ZSTD


# ---------------------------------------------------------------- containers (formats: nx2/shader_census.py)
def dictionaries(romfs):
    """The game's zstd dictionaries: Pack/ZsDic.pack.zs, a SARC-like pack of them"""
    path = os.path.join(romfs, "Pack", "ZsDic.pack.zs")
    if not os.path.exists(path):
        return []
    raw = zstd().decompress(open(path, "rb").read())
    start = struct.unpack_from("<I", raw, 0x0C)[0]
    out = []
    for k in range(struct.unpack_from("<H", raw, 0x1A)[0]):
        _, _, st, end = struct.unpack_from("<IIII", raw, 0x20 + k * 16)
        out.append(raw[start + st:start + end])
    return out


def unpack(data, dicts):
    return zstd().decompress(data, dicts)


def sarc_files(data):
    start = struct.unpack_from("<I", data, 0x0C)[0]
    count = struct.unpack_from("<H", data, 0x1A)[0]
    for k in range(count):
        _, _, st, end = struct.unpack_from("<IIII", data, 0x20 + k * 16)
        yield data[start + st:start + end]


def blobs(data, dicts):
    """The data itself, or each file when it is a SARC"""
    if data[:4] == b"SARC":
        for inner in sarc_files(data):
            yield from blobs(unpack(inner, dicts), dicts)
    else:
        yield data


def fnv(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def code_key(control, code):
    """vk_shader.cpp:135 code_key: FNV-1a 64 over the 0xd8 control section then the code area"""
    return fnv(bytes(code), fnv(bytes(control[:0xD8])))


def control_at(data, c):
    control = data[c:c + 0xD8]
    if len(control) < 0xD8 or control[:4] != CONTROL_MAGIC or struct.unpack_from("<H", control, 0x56)[0] > 7:
        return None, 0
    return control, struct.unpack_from("<I", control, 0x44)[0]


def stage(control, code):
    return struct.unpack_from("<H", control, 0x56)[0], control, code


def bnsh_programs(data, stats):
    if data[:4] != b"BNSH" or data[12:14] != b"\xff\xfe" or data[96:100] != b"grsc":
        raise ValueError("unsupported BNSH header")
    size = struct.unpack_from("<I", data, 28)[0]
    data = data[:size]
    count, variations = struct.unpack_from("<IQ", data, 124)
    for i in range(count):
        program = struct.unpack_from("<Q", data, variations + i * 64 + 16)[0]
        if not program or program + 80 > size:
            stats["bnsh:no-program"] += 1
            continue
        stages = []
        for rel in SLOTS:
            offset = struct.unpack_from("<Q", data, program + rel)[0]
            if not offset or offset + 32 > size:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from("<QQII", data, offset + 8)
            if not 0x60 <= control_size <= 0x1000 or control_off + control_size > size or code_off + code_size > size:
                continue
            control = data[control_off:control_off + control_size]
            if control[:4] != CONTROL_MAGIC:
                continue
            stats["bnsh:slot-%d" % rel] += 1
            stages.append(stage(control, data[code_off:code_off + code_size]))
        if stages:
            yield stages


def bnsh_all(data, stats):
    for m in re.finditer(b"BNSH", data):
        if data[m.start() + 96:m.start() + 100] == b"grsc":
            yield from bnsh_programs(data[m.start():m.start() + struct.unpack_from("<I", data, m.start() + 28)[0]], stats)


def bahs_programs(data, stats):
    base, _, blob = struct.unpack_from("<III", data, 0x14)
    table, count = struct.unpack_from("<II", data, base + 0x150)
    at, stages = base + 0x158, []
    for _ in range(count):
        step = struct.unpack_from("<I", data, at)[0]
        if step < 32:
            raise ValueError("stage at %#x" % at)
        offset, _, rel = struct.unpack_from("<QIi", data, at + 16)
        control, end = control_at(data, at + 16 + rel)
        if control is None or blob + offset + end > len(data):
            raise ValueError("stage at %#x" % at)
        stats["bahs:stage-%d" % struct.unpack_from("<H", control, 0x56)[0]] += 1
        stages.append(stage(control, data[blob + offset:blob + offset + end]))
        at += step
    table += base + 0x150
    at, heads = table + 8, []
    for _ in range(struct.unpack_from("<I", data, table + 4)[0]):
        size, _, kinds, first = struct.unpack_from("<4I", data, at)
        heads.append((first, bin(kinds).count("1")))
        at += size
    heads.sort()
    for i, (first, width) in enumerate(heads):
        last = heads[i + 1][0] if i + 1 < len(heads) else count
        for v in range(first, last, max(width, 1)):
            yield stages[v:v + width]


def ndsr_programs(data, base, stats):
    items = [struct.unpack_from("<II", data, base + 0x14 + 8 * i)
             for i in range(struct.unpack_from("<I", data, base + 0x0C)[0])]
    graphics = []
    for (c, ct), (x, xt) in zip(items, items[1:]):
        if ct & 15 not in (0, 3, 6) or xt & 15 != (ct & 15) + 1:
            continue
        control, end = control_at(data, base + c)
        if control is None or base + x + end > len(data):
            continue
        s = stage(control, data[base + x:base + x + end])
        stats["ndsr:stage-%d" % s[0]] += 1
        if ct & 15 == 6:
            yield [s]
        elif ct & 15 == 0:
            if graphics:
                yield graphics
            graphics = [s]
        elif graphics:
            graphics.append(s)
    if graphics:
        yield graphics


def ndve_programs(data, stats):
    if data[0x28:0x2C] == b"NDSR":
        yield from ndsr_programs(data, 0x28, stats)
    for m in re.finditer(re.escape(CONTROL_MAGIC), data):
        c = m.start()
        control, end = control_at(data, c)
        if control is None:
            continue
        for e in re.finditer(re.escape(struct.pack("<II", end, 0)), data):
            entry = e.start() - 8
            if entry < 0 or entry % 4:
                continue
            to_control, to_code = struct.unpack_from("<II", data, entry)
            table = c - to_control
            if not to_control or table < 0 or table % 4 or c + to_code + end > len(data):
                continue
            n, blobs_n = struct.unpack_from("<II", data, table)
            first = table + 8 + 8 * blobs_n
            if 0 < n < 256 and blobs_n < 16 and (entry - first) % 16 == 0 and 0 <= (entry - first) // 16 < n:
                stats["ndve:stage-%d" % stage(control, b"")[0]] += 1
                yield [stage(control, data[c + to_code:c + to_code + end])]
                break


def programs_in(data, stats):
    if data[:4] == b"BAHS":
        return bahs_programs(data, stats)
    if data[:4] == b"NDSR":
        return ndsr_programs(data, 0, stats)
    if data[:4] == b"NDVE":
        return ndve_programs(data, stats)
    return bnsh_all(data, stats)


def archives(romfs):
    if os.path.isfile(romfs):   # (a single archive or a directory of them)
        yield romfs
        return
    for folder in FOLDERS:
        for top, dirs, names in os.walk(os.path.join(romfs, folder)):
            dirs.sort()
            for name in sorted(names):
                if name.endswith(".zs") or ".bfsha" in name or ".sarc" in name or ".pck" in name:
                    yield os.path.join(top, name)


# ---------------------------------------------------------------- the seed + manifest
def enumerate_tree(romfs, verbose=True):
    dicts = dictionaries(romfs)
    stats = collections.Counter()
    stage_index, stage_list, programs, seen = {}, [], [], set()
    sources = []
    files = []
    for path in archives(romfs):
        rel = os.path.relpath(path, romfs if os.path.isdir(romfs) else os.path.dirname(path))
        made, found = 0, []
        try:
            for k, data in enumerate(blobs(unpack(open(path, "rb").read(), dicts), dicts)):
                try:
                    found += list(programs_in(data, stats))
                except Exception as e:
                    stats["container-skipped"] += 1
                    if verbose:
                        print("%-56s container %d %r skipped: %s" % (os.path.basename(path)[:56], k, data[:4], e))
        except Exception as e:
            stats["file-skipped"] += 1
            if verbose:
                print("%-56s skipped: %s" % (os.path.basename(path)[:56], e))
        for stages in found:
            ids = []
            for st, control, code in stages:
                end = struct.unpack_from("<I", control, 0x44)[0]
                control, code = control[:0xD8], code[:end] if end <= len(code) else code
                key = code_key(control, code)
                if key not in stage_index:
                    stage_index[key] = len(stage_list)
                    stage_list.append((key, st, control, code))
                    sources.append({"key": "%016x" % key, "stage": st, "bytes": len(code),
                                    "file": rel, "container": k})
                ids.append(stage_index[key])
            ids = tuple(sorted(set(ids)))
            if ids not in seen:
                seen.add(ids)
                programs.append(ids)
                made += 1
        files.append({"file": rel, "programs": made})
        if made and "Effect" not in path:
            print("%-56s %6d programs" % (os.path.basename(path)[:56], made))
    return {"dicts": len(dicts), "stats": dict(stats), "stages": stage_list, "programs": programs,
            "sources": sources, "files": files}


def write_seed(path, result):
    with open(path, "wb") as f:
        f.write(b"HKSEED01" + struct.pack("<II", len(result["programs"]), len(result["stages"])))
        for key, st, control, code in result["stages"]:
            f.write(struct.pack("<QIII", key, st, len(control), len(code)) + control + code)
        for ids in result["programs"]:
            f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))


def kinds_of(result):
    return collections.Counter(tuple(sorted(st for _, st, _, _ in
                                            (result["stages"][i] for i in ids))) for ids in result["programs"])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("romfs")
    p.add_argument("--out")
    p.add_argument("--manifest")
    p.add_argument("--quiet", action="store_true")
    a = p.parse_args(argv)
    r = enumerate_tree(a.romfs, verbose=not a.quiet)
    if a.out:
        write_seed(a.out, r)
    if a.manifest:
        with open(a.manifest, "w") as f:
            json.dump({"romfs": os.path.abspath(a.romfs), "dicts": r["dicts"], "stats": r["stats"],
                       "programs": len(r["programs"]), "stages": len(r["stages"]),
                       "kinds": {"+".join(map(str, k)): v for k, v in kinds_of(r).most_common()},
                       "files": r["files"], "stage_sources": r["sources"]}, f, indent=1)
    print("%d programs from %d stages (%d dictionary files)" % (len(r["programs"]), len(r["stages"]), r["dicts"]))
    print("by stages:", kinds_of(r).most_common(8))
    if a.out:
        print("wrote", a.out, os.path.getsize(a.out) >> 10, "KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
