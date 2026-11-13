"""All named and legacy numeric host/renderer option IDs must be distinct.

Compile cases from the actual API enums so implicit enum increments count too.
Keep pending Walk and Item wheel IDs reserved until they land (#2345, #2261).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    api = re.sub(r"/\*.*?\*/|//[^\n]*", "", api, flags=re.S)
    enums = re.findall(r"enum\s*\{[^}]*\}\s*;", api)
    declarations = [enum for enum in enums if "HOS_OPT_" in enum]
    names = re.findall(r"\bHOS_OPT_\w+\b", "\n".join(declarations))
    assert names, "No option declarations found"
    assert len(names) == len(set(names)), "Repeated option declaration"
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    option = replay.split("t.option =", 1)[1].split("};", 1)[0]
    numeric = re.findall(r"\bid\s*==\s*(\d+)\b", option)
    cases = names + numeric
    # Skip-title already owns 44; Walk and Item wheel are on concurrent branches.
    for name, value in (("HOS_OPT_SKIP_TITLE", 44), ("HOS_OPT_WALK_MODE", 49), ("HOS_OPT_WALK_SPEED", 50),
                        ("HOS_OPT_ITEM_WHEEL", 60)):
        if name not in names:
            cases.append(str(value))
    source = "\n".join(declarations) + "\nint option_id(int id) { switch (id) {\n"
    source += "\n".join(f"case {name}: return 0;" for name in cases)
    source += "\ndefault: return -1; } }\n"
    with tempfile.TemporaryDirectory(prefix="option-ids-") as temporary:
        path = Path(temporary) / "ids.c"
        obj = Path(temporary) / "ids.o"
        def compile(text):
            path.write_text(text, encoding="utf-8")
            return subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-c", str(path), "-o", str(obj)], capture_output=True, text=True)
        result = compile(source)
        assert result.returncode == 0, result.stderr
        # Prove the guard catches the original collision, an implicit enum value,
        # and a legacy numeric ID, not just collisions among explicit constants.
        for collision in (49, 50, 60, 33, 7):
            broken = source.replace("HOS_OPT_FOV = 51", f"HOS_OPT_FOV = {collision}")
            assert broken != source
            assert compile(broken).returncode != 0, f"Missed ID collision at {collision}"
    print(f"PASS: {len(cases)} option IDs are unique; explicit, implicit and legacy collisions rejected")


if __name__ == "__main__":
    main()
