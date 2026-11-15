"""What the shader caches' translator stamp is made of (nx2/translator_files.py, HOOKSHOT_SHADER_STAMP of nx2/build.py), run on
tiny authored files without building the renderer: a translator edit changes it; an edit to a file the translator does not read
(another renderer source, vk_api.h's host interface and menu ids) keeps it, so every player's translated programs are kept.

python -I -B tests/test_shader_stamp_inputs.py
"""
import importlib.util
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("translator_files_under_test", ROOT / "nx2" / "translator_files.py")
files = importlib.util.module_from_spec(spec)
spec.loader.exec_module(files)

build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
assert re.search(r"^import translator_files$", build, re.M), "nx2/build.py no longer imports translator_files"
assert 'shader_stamp = "-DHOOKSHOT_SHADER_STAMP=0x%sull" % translator_files.stamp(vk)' in build, \
    "nx2/build.py's HOOKSHOT_SHADER_STAMP is no longer translator_files.stamp(vk): update this check"
assert files.DIRS == ("eden", "sirit", "compat") and files.FILES == ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h"), \
    "the stamped files changed: say in nx2/translator_files.py why each one is in or out, then update this check"

with tempfile.TemporaryDirectory(prefix="shader-stamp-bank-") as tmp:
    vk = Path(tmp) / "gpu/vk"
    for rel in ("eden/a.cpp", "sirit/a.cpp", "compat/a.h", "vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h", "vk_draw.cpp"):
        f = vk / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"authored input\n")
    stamp = lambda: files.stamp(str(vk))
    first = stamp()
    assert re.fullmatch(r"[0-9a-f]{16}", first)
    (vk / "vk_draw.cpp").write_bytes(b"unrelated renderer edit\n")
    assert stamp() == first, "unrelated renderer source entered shader stamp"
    (vk / "vk_api.h").write_bytes(b"enum { HOS_OPT_AUDIO_OUTPUT = 57 };\n")
    assert stamp() == first, "a vk_api.h edit (the host interface, the menu's option ids) changed the shader stamp"
    (vk / "vk_bank.cpp").write_bytes(b"changed bank semantics\n")
    changed = stamp()
    assert changed != first, "bank-only edit did not invalidate shader stamp"
    (vk / "vk_bank.cpp").write_bytes(b"changed bank semantics\r\n")
    assert stamp() == changed, "existing CRLF normalization changed"
    for rel in ("vk_shader.cpp", "vk_state.h", "eden/a.cpp", "sirit/a.cpp", "compat/a.h"):
        (vk / rel).write_bytes(b"changed\n")
        assert stamp() != changed, "a %s edit kept the shader stamp" % rel
        changed = stamp()
    (vk / "eden/b.h").write_bytes(b"")
    assert stamp() != changed, "a file added to the recompiler kept the shader stamp"
    (vk / "eden/b.h").unlink()
    assert stamp() == changed
    assert files.stamped("gpu/vk/eden/shader_recompiler/x.cpp") and files.stamped("gpu/vk/vk_state.h")
    assert not files.stamped("gpu/vk/vk_api.h") and not files.stamped("gpu/vk/vk_draw.cpp")
    assert not files.stamped("gpu/vk/edenx/a.cpp") and not files.stamped("gpu/vk/vk_shader.cpp.orig")
print("PASS: the shader stamp changes for the translator's sources and keeps the caches for vk_api.h and other renderer sources")
