"""Game-free: the Linux AppImage carries the controller fonts the button prompts are drawn from (third_party/kenney_input,
nx2/prompt_assets.py KENNEY), and the keyboard face's (third_party/kenney_input_keyboard, KENNEY_KEYBOARD). Without them make_game.py says the prompts "were not made" and the game keeps the Switch's icons,
even with fontTools installed.

python -I tests/test_appimage_prompt_fonts.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
    assets = (ROOT / "nx2/prompt_assets.py").read_text(encoding="utf-8")
    folders = []
    for name in ("KENNEY", "KENNEY_KEYBOARD"):   # (the controllers' fonts, and the keyboard face's: nx2/prompt_keys.py)
        m = re.search(r'^%s = os\.path\.join\(os\.path\.dirname\(HERE\), "third_party", "([^"]+)"\)' % name, assets, re.M)
        if not m:
            raise SystemExit("FAIL: nx2/prompt_assets.py has no %s under third_party" % name)
        folder = "third_party/" + m.group(1)
        if not (ROOT / folder).is_dir():
            raise SystemExit("FAIL: %s does not exist" % folder)
        if not re.search(r'^cp -a %s "\$T/third_party/"$' % re.escape(folder), script, re.M):
            raise SystemExit("FAIL: scripts/linux/appimage.sh does not copy %s" % folder)
        folders.append(folder)
    print("ok: the AppImage copies %s" % " and ".join(folders))


if __name__ == "__main__":
    sys.exit(main())
