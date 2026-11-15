"""Translations reused across work folders (nx2/lift_store.py, HOOKSHOT_LIFT_STORE): a lift is saved whole under the share key
(make_game.py lift_share_key: the lift key without the work and game folders' paths), another folder takes it with the chunk
boundaries and hot pieces that lift left, a snapshot that is not whole or whose C is cut short is never used, and only the newest
are kept.

    python -I -B tests/test_lift_store.py
"""
import os
from pathlib import Path
import sys
import tempfile
import time
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import lift_store  # noqa: E402
import make_game  # noqa: E402


def lifted(work, chunk="80001000", body=b"void f_80001000(void) {\n}\n"):
    gen, image = work / "sa" / "gen", work / "sa" / "image"
    gen.mkdir(parents=True)
    image.mkdir(parents=True)
    (work / "image").mkdir(exist_ok=True)
    (gen / "code_000.c").write_bytes(body)
    (gen / "a64_table.c").write_text("int t;\n")
    (gen / ".lift_key").write_text("old key\n")
    (gen / "lifted_objects.txt").write_text("# lift old 1\n")
    (image / "image.bin").write_bytes(b"\0" * 64)
    (work / "image" / "chunks.txt").write_text(chunk + "\n")
    (work / "image" / "hot_pieces.txt").write_text("f_80001000 0\n")


with tempfile.TemporaryDirectory() as top:
    top = Path(top)
    store = top / "lifts"
    with mock.patch.dict(os.environ, {"HOOKSHOT_LIFT_STORE": "0"}):
        assert lift_store.root() is None
    a, b = top / "a" / "work", top / "b" / "work"
    lifted(a)
    assert lift_store.save(str(store), "k1", str(a))
    assert not lift_store.save(str(store), "k1", str(a))   # (one there already: kept as it is)
    snap = store / "k1"
    assert not (snap / "sa" / "gen" / ".lift_key").exists() and not (snap / "sa" / "gen" / "lifted_objects.txt").exists()
    assert (snap / "image" / "chunks.txt").read_text() == "80001000\n"

    # (another folder, with chunk boundaries of its own and stale C: it gets the lift's C, image and boundaries)
    (b / "sa" / "gen").mkdir(parents=True)
    (b / "sa" / "gen" / "code_999.c").write_text("stale\n")
    (b / "image").mkdir()
    (b / "image" / "chunks.txt").write_text("80009000\n")
    assert lift_store.restore(str(store), "k1", str(b))
    assert not (b / "sa" / "gen" / "code_999.c").exists()
    assert (b / "sa" / "gen" / "code_000.c").read_bytes() == (a / "sa" / "gen" / "code_000.c").read_bytes()
    assert (b / "sa" / "image" / "image.bin").read_bytes() == b"\0" * 64
    assert (b / "image" / "chunks.txt").read_text() == "80001000\n"
    assert (b / "image" / "hot_pieces.txt").read_text() == "f_80001000 0\n"
    (b / "sa" / "gen" / "code_000.c").write_bytes(b"edited in place\n")   # (a copy: the snapshot is not changed through it)
    assert (snap / "sa" / "gen" / "code_000.c").read_bytes() == b"void f_80001000(void) {\n}\n"

    # (a snapshot that is not whole is not used, and the folder keeps its own boundaries)
    (snap / "sa" / "gen" / "code_000.c").write_bytes(b"void f_80001000(void) {\n")
    c = top / "c" / "work"
    (c / "image").mkdir(parents=True)
    (c / "image" / "chunks.txt").write_text("8000a000\n")
    assert not lift_store.restore(str(store), "k1", str(c))
    assert (c / "image" / "chunks.txt").read_text() == "8000a000\n"
    assert not lift_store.restore(str(store), "missing", str(c))

    # (one whose C is cut short is let go)
    lifted(top / "d" / "work", body=b"void f_80001000(void) {\n")
    assert lift_store.save(str(store), "k2", str(top / "d" / "work"))
    gen_c = c / "sa" / "gen"
    assert not lift_store.restore(str(store), "k2", str(c), bad=lambda: make_game.lifted_cut_short(str(gen_c)))
    assert not (store / "k2").exists() and (c / "image" / "chunks.txt").read_text() == "8000a000\n"

    # (the newest kept, by last use; one used in the last hour stays whatever the count)
    now = time.time()
    for i, name in enumerate(("s1", "s2", "s3", "s4")):
        (store / name / "sa").mkdir(parents=True)
        os.utime(store / name, (now - (10 - i) * 86400, now - (10 - i) * 86400))
    os.utime(store / "k1", (now - 60, now - 60))
    lift_store.prune(str(store), keep=2, now=now)
    assert sorted(os.listdir(store)) == ["k1", "s4"], sorted(os.listdir(store))

    # (the share key: the same in two work folders and game folders, though the lift key is not; another setting changes it)
    exefs_a, exefs_b = a / "exefs", b / "exefs"
    for e in (exefs_a, exefs_b):
        e.mkdir()
        (e / "main").write_bytes(b"NSO0 one")
    for w in (a, b):
        (w / "image" / "chunks.txt").write_text("80001000\n")
        (w / "image" / "hot_pieces.txt").write_text("f_80001000 0\n")
        (w / "hot_funcs.txt").write_text("80001000\n")

    def env(w, out):
        return {"OOT_WORK": str(w), "OOT_EXEFS": str(w / "exefs"), "OOT_ROMFS": str(out / "romfs"), "OOT_DUMP": str(out),
                "RX_RELEASE": "1", "A64_HOT_CHUNK": "1", "A64_HOT_FUNCS": str(w / "hot_funcs.txt")}
    out_a, out_b = top / "a" / "game", top / "b" / "game"
    with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}, clear=True):
        share_a = make_game.lift_share_key(str(exefs_a), str(a), str(out_a), env(a, out_a))
        share_b = make_game.lift_share_key(str(exefs_b), str(b), str(out_b), env(b, out_b))
        assert share_a == share_b
        assert make_game.lift_key(str(exefs_a), str(a), env(a, out_a)) != make_game.lift_key(str(exefs_b), str(b), env(b, out_b))
        assert make_game.lift_share_key(str(exefs_b), str(b), str(out_b), dict(env(b, out_b), A64_HOT_CHUNK="")) != share_a
        (b / "image" / "chunks.txt").write_text("80002000\n")   # (other boundaries: another lift)
        assert make_game.lift_share_key(str(exefs_b), str(b), str(out_b), env(b, out_b)) != share_a
print("ok: a translation reused across work folders only when everything the lift reads matches")
