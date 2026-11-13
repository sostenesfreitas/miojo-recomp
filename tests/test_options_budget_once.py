"""Each key save() writes itself (gpu/vk/vk_menu.cpp) is one it owns, or the file's old line of it is kept too and, read
last, wins at the next start (memory_budget did).

python -I -B tests/test_options_budget_once.py
"""
from pathlib import Path
import re
import sys

menu = (Path(__file__).resolve().parents[1] / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
owned = set(re.findall(r'key == "([a-z_]+)"', re.search(r"bool mine = ([^;]+);", menu).group(1)))
written = set(re.findall(r'fprintf\(f, "([a-z_]+)=', menu))
missing = sorted(written - owned)
print(f"FAIL: save() writes {missing} but keeps the file's old lines of them too" if missing else f"ok: {sorted(written)}")
sys.exit(1 if missing or "memory_budget" not in written else 0)
