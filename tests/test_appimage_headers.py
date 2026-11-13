"""Game-free: every gpu/ header the host's sources include is one the Linux AppImage copies (scripts/linux/appimage.sh).
v2.0.6's AppImage lacked gpu/vk/oot_cheat_items.h, which host/hos_main.c includes, and no Linux build could finish (issue 990).

python -I tests/test_appimage_headers.py
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
    included = set()
    for src in sorted((ROOT / "host").rglob("*.[ch]")):
        for m in re.finditer(r'#include "\.\./gpu/((?:vk|render)/[^"]+)"', src.read_text(encoding="utf-8", errors="replace")):
            included.add(m.group(1))
    # headers those include, inside gpu/ (one level is all there is today; walk until nothing new)
    todo = list(included)
    while todo:
        rel = todo.pop()
        path = ROOT / "gpu" / rel
        if not path.is_file():
            raise SystemExit("host/ includes gpu/%s, which does not exist" % rel)
        for m in re.finditer(r'#include "([^"]+)"', path.read_text(encoding="utf-8", errors="replace")):
            dep = (Path(rel).parent / m.group(1)).as_posix()
            dep = str(Path(dep)).replace("\\", "/")
            parts = []
            for piece in dep.split("/"):
                if piece == "..":
                    parts.pop()
                elif piece != ".":
                    parts.append(piece)
            dep = "/".join(parts)
            if (ROOT / "gpu" / dep).is_file() and dep not in included:
                included.add(dep)
                todo.append(dep)
    copied_render = "cp -a gpu/render/*.h" in script
    copied_vk_loop = "grep -rhoE 'gpu/vk/[A-Za-z0-9_/]+[.]h' host" in script   # (with the folders below gpu/vk: compat/)
    missing = []
    for rel in sorted(included):
        if rel.startswith("render/") and rel.endswith(".h") and copied_render:
            continue
        if rel.startswith("vk/") and ("cp -a gpu/vk/%s" % rel[3:] in script or copied_vk_loop):
            continue
        missing.append(rel)
    if missing:
        raise SystemExit("the AppImage does not ship gpu/" + ", gpu/".join(missing) + " (host/ includes them)")
    print("ok: the AppImage ships the %d gpu headers host/ includes" % len(included))


if __name__ == "__main__":
    main()
