"""A spare staging arena is made and freed off the replay thread, on cards that keep 256 MB of arenas.

Making a 64 MB staging arena took the replay thread 6-66 ms on an RTX 3060 (Linux), up to ~360 ms with VRAM nearly full, and
the 30 s idle rule and the 256 MB cap freed arenas that the next streamed texture made again in the middle of play. The
spare (SpareArenas in gpu/vk/vk_texture.cpp) is taken by staging() before it makes one, a worker makes the next, and an idle
arena that would be freed becomes the spare or is freed on the worker. Cards that keep 64 MB are as before.

python -I -B tests/test_staging_spare.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(cond, why):
    if not cond:
        raise SystemExit(why)


def main():
    tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")

    on = block(tex, "bool spare_on() {")
    check("RX_VK_STAGING_SPARE" in on, "no RX_VK_STAGING_SPARE switch")
    check("staging_keep() > kArena" in on, "the spare is not limited to cards that keep more than one arena")

    stage = block(tex, "Staging staging(uint64_t n, uint64_t align = 256) {")
    take = stage.index("spare_take(a)")
    check(take < stage.index("make(std::max(kArena, need))"), "staging() makes an arena before it takes the spare")
    check("need <= kArena && spare_take(a)" in stage, "a request larger than an arena takes the 64 MB spare")
    # (a reused arena goes before the spare: the spare only replaces a make)
    check(stage.index("a.value <= completed_value") < take, "the spare is taken before an idle arena is reused")

    taken = block(tex, "bool spare_take(Arena& out) {")
    check("std::lock_guard lk(s.mu);" in taken, "the spare is taken without its lock")
    check("spare_wake(s, true);" in taken, "taking the spare does not ask the worker for the next")

    give = block(tex, "bool spare_give_back(const Arena& a) {")
    check("a.size == kArena" in give, "an arena of another size becomes the spare")
    check("s.to_free.push_back(a);" in give, "an arena let go is not freed on the worker")

    worker = block(tex, "void spare_worker() {")
    check(worker.index("lk.unlock();") < worker.index("destroy_buffer(") < worker.index("lk.lock();"),
          "the worker frees arenas while it holds the lock")
    check(worker.index("lk.unlock();") < worker.index("arena_make(a, kArena)") < worker.index("lk.lock();"),
          "the worker makes the spare while it holds the lock")
    check("else s.to_free.push_back(a);" in worker, "a spare made while another came back is kept (a leak)")

    frame = tex[tex.index("const uint64_t keep = staging_keep();"):]
    frame = frame[:frame.index("kept += it->size;")]
    check("if (!spare_give_back(*it)) destroy_buffer(it->buffer, it->allocation);" in frame,
          "texture_frame frees idle arenas on the replay thread with the spare on")
    print("the spare staging arena is made and freed off the replay thread: ok")


if __name__ == "__main__":
    main()
