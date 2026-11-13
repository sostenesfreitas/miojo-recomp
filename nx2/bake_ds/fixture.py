"""A synthetic game tree for the local bake (bake-deepseek): real NVN2 containers, tiny hand-built programs.

    python3 nx2/bake_ds/fixture.py <dir> [--with-dict]

Nothing here comes from a game: the containers are the formats nx2/shader_census.py reads (BNSH, BAHS,
NDSR, NDVE, a .vsetb BYML, a SARC wrapper, zstd .zs with and without a dictionary), and the programs are
the two Ampere encodings a lowering probe pinned (EXIT; MOV.imm + EXIT). It exists so the whole bake can
run end to end without the player's romfs, and so the enumerator can be compared against the census's
own readers on the same bytes.

The tree:
  Shader/Test.bfsha.zs                    BNSH: (vertex, fragment) + compute
  Shader/ApplicationPackage/Test.sarc.zs  SARC: BNSH + BAHS
  Lib/agl/agl_test.pck                    BAHS: (vertex, fragment) + compute
  Lib/agl/agl_ndsr_test.pck               NDSR: (vertex, fragment) + compute
  Movie/Ndve/Ndve_test.pck                NDVE: an NDSR at +0x28
  Effect/particle.vsetb.byml.zs           BYML with a BNSH
  Shader/Bones.bfsha                     BNSH whose vertex program reads generic attributes 6 and 7
                                          (the bone variants seed_program makes: inputs 1 and 2)
  Shader/Dict.bfsha.zs                    only with --with-dict: zstd with a raw dictionary, plus
                                          Pack/ZsDic.pack.zs as the dictionary pack the census reads
"""
import argparse
import os
import struct
import subprocess
import sys

CTL = 0xD8
CONTROL_MAGIC = 0x98761234
EXIT = struct.pack("<IIII", 0x794D, 0, 7 << 23, 0)
MOV = struct.pack("<IIII", 0x802 | (2 << 16), 0x3F800000, 0x00000F00, 0)


def control(stage, code_len, local=(1, 1, 1), shared=0, push=0, ubo=0, ssbo=0, tex=0, img=0, topology=0):
    c = bytearray(CTL)
    struct.pack_into("<I", c, 0, CONTROL_MAGIC)
    struct.pack_into("<I", c, 0x38, code_len)
    struct.pack_into("<I", c, 0x40, code_len)   # c1 at the end: none
    struct.pack_into("<I", c, 0x44, code_len)
    struct.pack_into("<H", c, 0x56, stage)
    struct.pack_into("<III", c, 0x58, *local)
    struct.pack_into("<I", c, 0x64, shared)
    c[0xA9] = push
    c[0xB1], c[0xB2], c[0xB3], c[0xB6] = ubo, ssbo, tex, img
    c[0x5F] = topology
    return bytes(c)


def sph(generics=()):
    h = bytearray(0x80)
    # (ProgramHeader: common0..4 are 0x14 bytes, then the vtg union: 3 pad + imap_systemb, so
    #  vtg.imap_generic_vector is at 0x18; a nibble per generic, bits 0..3 = used components)
    for g in generics:
        h[0x18 + g // 2] |= 0xF << ((g % 2) * 4)
    return bytes(h)


def program(stage, generics=(), compute=False, **kw):
    """A stage package: control section + code area. Graphics need two instructions (one is 'header-only')."""
    code = (EXIT if compute else sph(generics) + MOV + EXIT)
    ctl = control(stage, len(code), **kw)
    return ctl + code


def mesh_pair(topology=3, threads=64, max_vertices=128, max_primitives=64):
    """A task and a mesh stage as NVN2 gives them: threads/payload at +0x58, max primitives/vertices at +0x60,
    the output topology at +0x5f (1 points, 3 triangles)"""
    task = program(6, local=(threads | (128 << 16), 0, 0), topology=topology)
    mesh = program(7, local=(threads | (128 << 16), 0, max_primitives | (max_vertices << 16)), topology=topology)
    return [task, mesh]


def graphics_pair(vgen=(), vid=0, fid=0):
    return [program(0, vgen, local=(vid, 0, 0)), program(1)]   # (locals unused for graphics)


def compute(local=(8, 4, 1), shared=1024, push=0, ubo=0, ssbo=0, tex=0, img=0):
    return [program(5, compute=True, local=local, shared=shared, push=push, ubo=ubo, ssbo=ssbo, tex=tex, img=img)]


# HMMA.16816.F16 (Ampere opcode 0x23c): dest R4, A R8, B R20, C 255 (zero). The lowering refuses it unless the
# tensor path is on (native_matrix_multiply) or EDEN_NX2_TENSOR is set (lower_to_maxwell.h:1621-1632), so it is
# the probe of the target's tensor feature. The following STS [R2], R4 stores the result to shared memory, so
# D is live and the multiply survives DCE (without a consumer the recompiler drops it and neither form emits a
# matrix op; that made the first version of this probe unable to tell the two forms apart).
HMMA = struct.pack("<IIII", 0x23C | (4 << 16) | (8 << 24), 20, 0x800 | 0xFF, 0)
# STS [R2], R4: opcode 0x388, address R2 (bits 24-31), data R4 (bits 32-39), .32 (width 2), no stride.
STS = struct.pack("<IIII", 0x388 | (2 << 24), 4, 0x400, 0)


def compute_hmma(local=(8, 4, 1), shared=1024):
    """A compute stage with one native matrix multiply, its result stored to shared memory, then EXIT"""
    code = HMMA + STS + EXIT
    return [control(5, len(code), local=local, shared=shared) + code]


def build_tensor(dir):
    """The tensor-gated program on its own: one BNSH, one compute stage (no game data)."""
    os.makedirs(os.path.join(dir, "Shader"), exist_ok=True)
    with open(os.path.join(dir, "Shader/Matrix.bfsha"), "wb") as f:
        f.write(bnsh([compute_hmma()]))


# ---------------------------------------------------------------- containers
def bnsh(variations):
    """A BNSH whose variations are lists of stage packages"""
    out = bytearray(0x200)
    out[:4] = b"BNSH"
    out[12:14] = b"\xff\xfe"
    out[96:100] = b"grsc"
    var_at = 0x200
    out += bytes(64 * len(variations))
    programs_at = len(out)
    out += bytes(80 * len(variations))
    struct.pack_into("<I", out, 28, 0)   # size, after everything is placed
    for i, stages in enumerate(variations):
        struct.pack_into("<Q", out, var_at + i * 64 + 16, programs_at + i * 80)
        slots = (16, 24, 32, 40, 48, 56, 64, 72)
        for k, pkg in enumerate(stages):
            rec = len(out)
            ctl, code = pkg[:CTL], pkg[CTL:]
            out += bytes(40)                       # (u64 offset's first 8 + control/code/sizes)
            control_off = len(out)
            out += ctl
            code_off = len(out)
            out += code
            struct.pack_into("<QQII", out, rec + 8, control_off, code_off, len(code), len(ctl))
            struct.pack_into("<Q", out, programs_at + i * 80 + 8 + k * 8, rec)
    struct.pack_into("<I", out, 28, len(out))
    struct.pack_into("<IQ", out, 124, len(variations), var_at)
    return bytes(out)


def bahs(program_stages, kinds):
    """program_stages: list of lists of stage packages; kinds: the bit mask of each program's stage kinds
    (a program's run of stages is width = popcount(kinds), one stage per kind)"""
    base = 0x200
    all_stages = [s for p in program_stages for s in p]
    n = len(all_stages)
    names = [b"prog%d\0" % k for k in range(len(program_stages))]
    entries_size = sum(16 + len(name) for name in names)
    table_rel = 8 + 32 * n                       # (the stage records come first)
    table_at = base + 0x150 + table_rel
    controls_at = table_at + 8 + entries_size + 16
    blob_at = controls_at + CTL * n
    out = bytearray(blob_at)
    out[:4] = b"BAHS"
    struct.pack_into("<III", out, 0x14, base, 0, blob_at)   # blob size filled below
    struct.pack_into("<II", out, base + 0x150, table_rel, n)
    blob = bytearray()
    for k, pkg in enumerate(all_stages):
        ctl, code = pkg[:CTL], pkg[CTL:]
        ctl_at = controls_at + k * CTL
        out[ctl_at:ctl_at + CTL] = ctl
        code_off = len(blob)
        blob += code
        at = base + 0x158 + k * 32
        struct.pack_into("<I", out, at, 32)
        struct.pack_into("<I", out, at + 4, 0)   # kind is in the program table
        struct.pack_into("<Q", out, at + 16, code_off)
        struct.pack_into("<i", out, at + 28, ctl_at - (at + 16))
    at = table_at + 8
    first = 0
    for name, stages, kind in zip(names, program_stages, kinds):
        struct.pack_into("<IIII", out, at, 16 + len(name), len(name), kind, first)
        out[at + 16:at + 16 + len(name)] = name
        at += 16 + len(name)
        first += len(stages)
    struct.pack_into("<I", out, table_at + 4, len(program_stages))
    struct.pack_into("<I", out, 0x18, len(blob))
    return bytes(out + blob)


def ndsr(program_stages):
    """items: for each program, control then code with the types the reader pairs (0/1, 3/4, 6/7)"""
    nstages = sum(len(s) for s in program_stages)
    out = bytearray(0x14)
    out[:4] = b"NDSR"
    items = []
    at = 0x14 + 16 * nstages   # (the item table comes first)
    body = bytearray()
    js = []
    for stages in program_stages:
        for pkg in stages:
            ctl, code = pkg[:CTL], pkg[CTL:]
            st = struct.unpack_from("<H", ctl, 0x56)[0]
            ctype = {0: 0, 1: 3, 5: 6}[st]
            c_off = at + len(body)
            body += ctl
            x_off = at + len(body)
            body += code
            js.append((c_off, ctype, x_off, ctype + 1))
    struct.pack_into("<I", out, 0x0C, 2 * len(js))   # (each stage is a control item and a code item)
    for c_off, ctype, x_off, xtype in js:
        items.append(struct.pack("<II", c_off, (0 << 4) | ctype))
        items.append(struct.pack("<II", x_off, (0 << 4) | xtype))
    out += b"".join(items) + body
    return bytes(out)


def ndve(net):
    head = bytearray(0x28)
    head[:4] = b"NDVE"
    return bytes(head) + net


def byml_with_bnsh(inner):
    out = bytearray(b"BYML")
    out += bytes(0xFC)   # (the BNSH starts at 0x100: bnsh_all scans for it)
    out += inner
    return bytes(out)


def sarc(files):
    names = b""
    out = bytearray(0x20 + 16 * len(files))
    out[:4] = b"SARC"
    data_at = 0x20 + 16 * len(files)
    body = bytearray()
    for k, (name, data) in enumerate(files):
        start = len(body)
        body += data
        struct.pack_into("<IIII", out, 0x20 + k * 16, 0, 0, start, len(body))
    struct.pack_into("<I", out, 0x0C, data_at)
    struct.pack_into("<H", out, 0x1A, len(files))
    return bytes(out + body)


def dic_pack(dicts):
    out = bytearray(0x20 + 16 * len(dicts))
    out[:4] = b"SARC"
    start = 0x20 + 16 * len(dicts)
    body = bytearray()
    for k, d in enumerate(dicts):
        st = len(body)
        body += d
        struct.pack_into("<IIII", out, 0x20 + k * 16, 0, 0, st, len(body))
    struct.pack_into("<I", out, 0x0C, start)
    struct.pack_into("<H", out, 0x1A, len(dicts))
    return bytes(out + body)


# ---------------------------------------------------------------- the tree
def build(dir, with_dict=False):
    def write(rel, data):
        path = os.path.join(dir, rel)
        os.makedirs(os.path.dirname(path) or dir, exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def z(data, dictionary=None):
        try:
            import zstandard
        except ImportError:
            pass
        else:
            d = zstandard.ZstdCompressionDict(dictionary) if dictionary is not None else None
            return zstandard.ZstdCompressor(dict_data=d).compress(data)
        if dictionary is not None:
            import tempfile
            with tempfile.NamedTemporaryFile(delete=False) as tf:
                tf.write(dictionary)
                path = tf.name
            try:
                return subprocess.run(["zstd", "-q", "-c", "-D", path], input=data,
                                      stdout=subprocess.PIPE, check=True).stdout
            finally:
                os.unlink(path)
        try:
            from compression import zstd   # (Python 3.14)
            return zstd.compress(data)
        except ImportError:
            return subprocess.run(["zstd", "-q", "-c"], input=data, stdout=subprocess.PIPE, check=True).stdout

    write("Shader/Test.bfsha.zs", z(bnsh([graphics_pair(vid=1), compute(local=(8, 4, 1))])))
    inner_bnsh = bnsh([graphics_pair(vid=2)])
    inner_bahs = bahs([graphics_pair(vid=3), compute(local=(4, 2, 1), shared=2048)], kinds=[0b11, 0b1000])
    write("Shader/ApplicationPackage/Test.sarc.zs", z(sarc([("a.bnsh", inner_bnsh), ("b.bahs", inner_bahs)])))
    write("Lib/agl/agl_test.pck", bahs([graphics_pair(vid=4), graphics_pair(vid=5), compute(local=(16, 1, 1)),
                                        mesh_pair() + graphics_pair(vid=11)[1:]],
                                       kinds=[0b11, 0b11, 0b1000, 0b110010]))
    write("Lib/agl/agl_ndsr_test.pck", ndsr([graphics_pair(vid=6), compute(local=(2, 2, 1))]))
    write("Movie/Ndve/Ndve_test.pck", ndve(ndsr([graphics_pair(vid=7), compute(local=(1, 1, 1), shared=4096)])))
    write("Effect/particle.vsetb.byml.zs", z(byml_with_bnsh(bnsh([graphics_pair(vid=8)]))))
    write("Shader/Bones.bfsha", bnsh([graphics_pair(vgen=(6, 7), vid=9)]))

    stats = {"dict": "skipped"}
    if with_dict:
        # (a raw dictionary; libzstd accepts raw content dictionaries, which is what the pack holds)
        d = (b"bake-ds-dictionary " * 256)[:4096]
        write("Pack/ZsDic.pack.zs", z(dic_pack([d])))
        data = bnsh([graphics_pair(vid=10)])
        comp = z(data, d)
        if comp[:4] != b"\x28\xb5\x2f\xfd":
            raise ValueError("dictionary compressor did not produce a zstd frame")
        write("Shader/Dict.bfsha.zs", comp)
        stats["dict"] = "written"
    return stats


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("dir")
    p.add_argument("--with-dict", action="store_true")
    a = p.parse_args(argv)
    os.makedirs(a.dir, exist_ok=True)
    print(build(a.dir, a.with_dict))
    return 0


if __name__ == "__main__":
    sys.exit(main())
