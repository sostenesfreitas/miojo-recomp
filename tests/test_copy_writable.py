"""make_game copies the profile files and the window icon from a read-only install as writable files, and leaves a file
that is already its own copy alone.

From a read-only tools folder (a Nix store, a packaged install) shutil.copy2 took the read-only mode along, and lift.py
then failed to write work/image/chunks.txt (PermissionError), on that build and every later one; the second build
of a game folder stopped on its read-only hookshot.png the same way. With --work the checkout's own work folder,
work/image/chunks.txt was copied onto itself and every build stopped at once with shutil.SameFileError.
python -I -B tests/test_copy_writable.py
"""
import importlib.util
import os
from pathlib import Path
import stat
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


def main():
    text = SOURCE.read_text(encoding="utf-8")
    if 'shutil.copy2(src, os.path.join(work, "image", "chunks.txt"))' in text:
        raise SystemExit("chunks.txt is copied with its source's mode again")
    if 'shutil.copy2(icon, os.path.join(out, "hookshot.png"))' in text:
        raise SystemExit("the window icon is copied with its source's mode again (the next build cannot overwrite it)")
    with tempfile.TemporaryDirectory(prefix="hookshot-copy-test-") as d:
        src, dst = os.path.join(d, "src.txt"), os.path.join(d, "dst.txt")
        with open(src, "w") as f:
            f.write("new")
        with open(dst, "w") as f:
            f.write("old")
        os.chmod(src, stat.S_IREAD)
        os.chmod(dst, stat.S_IREAD)   # (a copy left read-only by an earlier build)
        make_game.copy_writable(src, dst)
        with open(dst, "a") as f:     # (what lift.py does to chunks.txt)
            f.write("+")
        with open(dst) as f:
            if f.read() != "new+":
                raise SystemExit("the copy does not hold the source's bytes")
        os.chmod(src, stat.S_IREAD | stat.S_IWRITE)
        make_game.copy_writable(src, src)   # (--work is the checkout's own work folder)
        link = os.path.join(d, "link.txt")
        os.link(src, link)
        make_game.copy_writable(src, link)   # (the same file by another name)
        with open(src) as f:
            if f.read() != "new":
                raise SystemExit("a file copied onto itself lost its bytes")
    print("PASS: the work folder's copies stay writable, and a file that is its own copy is left alone")


if __name__ == "__main__":
    main()
