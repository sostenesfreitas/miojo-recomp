"""A staging arena that cannot be made at 64 MB is made at the size the upload asks for.

Without resizable BAR a staging arena is new system memory the driver pins. On a 16 GB laptop with RAM nearly gone (the
game's pools imported), no 64 MB arena could be made even for a 36 KB upload once the idle ones had been given back, and
every texture upload failed until one was free: BC3 textures drawn as noise on an RX 5500M. staging() now tries one of the
size asked for, and says so in the log.

python -I -B tests/test_staging_arena_fallback.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    if "Staging staging(uint64_t n, uint64_t align = 256) {" not in tex:
        raise SystemExit("vk_texture.cpp has no staging()")
    body = block(tex, "Staging staging(uint64_t n, uint64_t align = 256) {")
    if "const bool small = need < kArena && make(need);" not in body:
        raise SystemExit("a staging arena that cannot be made at 64 MB is not tried at the size asked for")
    if body.index("make(std::max(kArena, need))") > body.index("make(need)"):
        raise SystemExit("the small arena is tried before the 64 MB one")
    # (staging() makes its arenas through arena_make, which the spare's worker shares: the unmapped one is destroyed there)
    if "return arena_make(a, size);" not in body:
        raise SystemExit("staging() does not make its arenas through arena_make")
    made = block(tex, "bool arena_make(Arena& a, uint64_t size) {")
    if "if (!m) {" not in made or "destroy_buffer(a.buffer, a.allocation);" not in made:
        raise SystemExit("an arena made but not mapped is kept (a leak)")
    if "no %llu MB arena could be made" not in body:
        raise SystemExit("a staging arena that could not be made is not logged")
    print("a staging arena that cannot be made at 64 MB is made at the size asked for: ok")


if __name__ == "__main__":
    main()
