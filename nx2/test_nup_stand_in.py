"""Damaged .zs files in an unpacked romfs and their stand-ins: python -I -B nx2/test_nup_stand_in.py

No game bytes: small made-up files in a temporary folder. Requires zstandard.
"""
import importlib.util
import os
import sys
import tempfile

import zstandard

RUN_ALL_MODULES = ["zstandard"]

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tested_nup", os.path.join(HERE, "nup.py"))
nup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nup)

ZSTD = b"\x28\xb5\x2f\xfd"
failures = []


def check(what, got, want):
    if got != want:
        failures.append("%s: %.200r, %.200r expected" % (what, got, want))


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


with tempfile.TemporaryDirectory() as romfs:
    tex = os.path.join(romfs, "Tex")
    # (as in the 23.3 GB dump: the start of the file is zeros, the rest is there)
    write(os.path.join(tex, "Rock_Temple_Gate_M_01_Nrm.bntx.zs"), bytes(400) + b"rest" * 30)
    write(os.path.join(tex, "Rock_Temple_Gate_P_01_Nrm.bntx.zs"), ZSTD + b"P" * 500)    # the nearest name: chosen
    write(os.path.join(tex, "Rock_Temple_Gate_M_01_Alb.bntx.zs"), ZSTD + b"A" * 4000)   # another kind of texture
    write(os.path.join(tex, "Rock_Temple_Gate_A_Nrm.bntx.zs"), ZSTD + b"A" * 4000)      # as close a name, a size further off
    write(os.path.join(tex, "Rock_Temple_Gate_M_02_Nrm.bntx.zs"), bytes(64))            # damaged too: never a stand-in
    write(os.path.join(tex, "Bake_Stage_G_2_1_d0_t0.bntx.zs"), bytes(64))
    write(os.path.join(tex, "Bake_Stage_G_2_2_d0_t0.bntx.zs"), ZSTD + b"t" * 60)
    write(os.path.join(tex, "Bake_Stage_G_2_2_d1_un3.bntx.zs"), ZSTD + b"u" * 60)
    write(os.path.join(romfs, "Lonely", "Only_One_Nrm.bntx.zs"), bytes(16))             # nothing of its kind beside it
    write(os.path.join(romfs, "Other", "plain.txt"), bytes(16))                         # not a .zs: not looked at
    write(os.path.join(romfs, "Other", "Skip_A.bntx.zs"), b"\x5a\x2a\x4d\x18" + bytes(8) + ZSTD)  # a skippable frame first
    # (#2361: still in the .nup's frame, as builders before #456 wrote them: content as large as the file, then other bytes)
    def wrapped(content):
        frame = zstandard.ZstdCompressor().compress(content)
        return frame + b"x" * (len(content) - len(frame))
    zeros = wrapped(bytes(1000))
    header = zstandard.frame_header_size(zeros)
    healthy = zstandard.ZstdCompressor().compress(b"BNTX" + b"wall" * 500)
    write(os.path.join(tex, "Wall_Gate_M_01_Nrm.bntx.zs"), zeros)
    write(os.path.join(tex, "Wall_Gate_M_02_Nrm.bntx.zs"), zeros[:header] + b"\xff" * (1000 - header))  # does not decompress
    write(os.path.join(tex, "Wall_Gate_P_01_Nrm.bntx.zs"), healthy)
    write(os.path.join(tex, "Wall_Gate_W_01_Alb.bntx.zs"), wrapped(b"BNTX" * 250))     # not zeros: left as it is

    damaged = sorted(os.path.relpath(p, romfs) for p in nup.damaged_game_files(romfs))
    check("damaged", damaged, sorted(os.path.join(*p) for p in (
        ("Lonely", "Only_One_Nrm.bntx.zs"), ("Tex", "Bake_Stage_G_2_1_d0_t0.bntx.zs"),
        ("Tex", "Rock_Temple_Gate_M_01_Nrm.bntx.zs"), ("Tex", "Rock_Temple_Gate_M_02_Nrm.bntx.zs"),
        ("Tex", "Wall_Gate_M_01_Nrm.bntx.zs"), ("Tex", "Wall_Gate_M_02_Nrm.bntx.zs"))))

    done = {os.path.relpath(p, romfs): o and os.path.basename(o) for p, o in nup.stand_in_damaged(romfs)}
    check("normal map", done.get(os.path.join("Tex", "Rock_Temple_Gate_M_01_Nrm.bntx.zs")), "Rock_Temple_Gate_P_01_Nrm.bntx.zs")
    check("second normal map", done.get(os.path.join("Tex", "Rock_Temple_Gate_M_02_Nrm.bntx.zs")), "Rock_Temple_Gate_P_01_Nrm.bntx.zs")
    check("bake", done.get(os.path.join("Tex", "Bake_Stage_G_2_1_d0_t0.bntx.zs")), "Bake_Stage_G_2_2_d0_t0.bntx.zs")
    check("alone", done.get(os.path.join("Lonely", "Only_One_Nrm.bntx.zs"), "missing"), None)
    check("wrapped zeros", done.get(os.path.join("Tex", "Wall_Gate_M_01_Nrm.bntx.zs")), "Wall_Gate_P_01_Nrm.bntx.zs")
    check("wrapped, not decompressing", done.get(os.path.join("Tex", "Wall_Gate_M_02_Nrm.bntx.zs")), "Wall_Gate_P_01_Nrm.bntx.zs")
    with open(os.path.join(tex, "Rock_Temple_Gate_M_01_Nrm.bntx.zs"), "rb") as f:
        check("replaced bytes", f.read(), ZSTD + b"P" * 500)
    with open(os.path.join(tex, "Wall_Gate_M_01_Nrm.bntx.zs"), "rb") as f:
        check("replaced wrapped bytes", f.read(), healthy)
    check("no .part left", [n for _, _, fs in os.walk(romfs) for n in fs if n.endswith(".part")], [])
    check("a second pass", [os.path.relpath(p, romfs) for p, _ in nup.stand_in_damaged(romfs)],
          [os.path.join("Lonely", "Only_One_Nrm.bntx.zs")])
    check("zstd starts", [nup.zstd_start(h) for h in (ZSTD, b"\x50\x2a\x4d\x18", b"\x5f\x2a\x4d\x18", b"\x60\x2a\x4d\x18",
                                                       bytes(4), ZSTD[:3])], [True, True, True, False, False, False])

# make_game's step: only in a romfs the builder unpacked (its stamp), once per unpack
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), "third_party", "a64-kit", "tools")]
mg_spec = importlib.util.spec_from_file_location("tested_make_game", os.path.join(HERE, "make_game.py"))
make_game = importlib.util.module_from_spec(mg_spec)
mg_spec.loader.exec_module(make_game)
calls = []
reported = []   # (whether each call was given a progress(done, total): the check says how far it is, #1764)
sys.modules["nup"] = type(sys)("nup")
sys.modules["nup"].stand_in_damaged = lambda romfs, progress=None: calls.append(romfs) or reported.append(callable(progress)) or []
with tempfile.TemporaryDirectory() as out:
    romfs = os.path.join(out, "romfs")
    os.makedirs(romfs)
    make_game.stand_in_damaged_files(out, romfs)
    check("no stamp: not the builder's romfs", calls, [])
    with open(os.path.join(out, ".hookshot_romfs"), "w") as f:
        f.write("unpacked by this builder: game.nup 1\n")
    make_game.stand_in_damaged_files(out, romfs)
    make_game.stand_in_damaged_files(out, romfs)
    check("checked once", calls, [romfs])
    with open(os.path.join(out, ".hookshot_romfs"), "w") as f:
        f.write("unpacked by this builder: other.nup 2\n")
    make_game.stand_in_damaged_files(out, romfs)
    check("checked again for another unpack", calls, [romfs, romfs])
    with open(os.path.join(out, ".hookshot_romfs"), "rb") as f:
        stamp = f.read()
    with open(os.path.join(out, ".hookshot_romfs_checked"), "wb") as f:
        f.write(b"resolved-v1\n" + stamp)   # (as builders before #2361 wrote it)
    make_game.stand_in_damaged_files(out, romfs)
    make_game.stand_in_damaged_files(out, romfs)
    check("an older check: checked again, once", calls, [romfs, romfs, romfs])
    check("each check reports its progress", reported, [True, True, True])

if failures:
    print("\n".join(failures))
    sys.exit(1)
print("stand-ins: ok")
