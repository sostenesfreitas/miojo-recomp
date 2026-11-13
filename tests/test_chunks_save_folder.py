"""chunks.save makes the folder it records the boundaries in (nx2/chunks.py).

`python -I nx2/lift.py --standalone --write` writes the boundaries to <work>/image/chunks.txt, while the image goes
to <work>/sa/image; on a fresh work folder nothing had made <work>/image, and the lift stopped after its translation
with FileNotFoundError, at the last step (make_game.py prepares the folder, the documented command did not).

python -I -B tests/test_chunks_save_folder.py
"""
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import chunks  # noqa: E402


def main():
    with tempfile.TemporaryDirectory(prefix="hookshot-chunks-save-") as temp:
        # (a standalone lift's folder: <work>/image, with <work> existing and image/ not)
        work = Path(temp) / "work"
        work.mkdir()
        image = work / "image"
        assert not image.exists(), "the fixture must start without the folder"
        chunks.save(str(image), [0x80002000, 0x80001000])
        path = image / chunks.FILE
        assert path.is_file(), "chunks.save did not make its folder: %s" % path
        assert chunks.load(str(image)) == [0x80001000, 0x80002000], chunks.load(str(image))
        # (as lift.py does on a later lift: the folder is there and the file is replaced, not appended)
        chunks.save(str(image), [0x80003000])
        assert chunks.load(str(image)) == [0x80003000], chunks.load(str(image))
    print("chunks.save makes <work>/image and writes the boundaries: ok")


if __name__ == "__main__":
    main()
