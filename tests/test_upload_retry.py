"""A texture whose upload failed is tried again, whatever its kind (gpu/vk/vk_texture.cpp texture_touch).

With the VRAM or RAM full an upload can fail for want of staging memory. #1505 made a BC3 texture try again 30 frames on;
every other kind was marked uploaded on a failure: a first upload that failed left an empty image, drawn as it was until the
game wrote that memory again, and a re-upload that failed dropped what the CPU had written (upload_gen moved past it) and
the GPU buffer write gpu_wrote_into had just marked seen.

texture_touch is too tied to the renderer to run here, so its source is checked:
- the failed branch marks nothing done (no uploaded, no upload_gen), sets retry_frame 30 frames on, puts gpu_wrote_seen back,
  and logs once per texture kind (40 lines);
- the wait stands right after the epoch check, before gpu_wrote_into and the dirty scan, so a waiting texture loses nothing;
- the value of gpu_wrote_seen is taken before gpu_wrote_into;
- a successful upload clears retry_frame;
- a texture evicted and made again over storage render targets took, whose entry (upload it from memory) was taken before
  the upload, gets the entry back, so the retry does not take it for rendered storage and mark it uploaded empty;
- an image no upload takes (upload_never, the checks upload_inner starts with) is still marked uploaded, not tried forever.
No game or GPU required.

python -I -B tests/test_upload_retry.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    src = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_texture.h").read_text(encoding="utf-8")
    if "uint64_t retry_frame = 0;" not in header:
        raise SystemExit("FAIL: a texture has no frame to try a failed upload again from (only BC3 ones did)")
    touch = block(src, "Texture* texture_touch(")
    tail = touch[touch.index("    if (up_ok) {"):]
    ok = tail[:tail.index("    } else")]
    if "tex->retry_frame = 0;" not in ok:
        raise SystemExit("FAIL: a successful upload leaves the texture waiting to try again")
    if "} else if (upload_never(*tex)) {" not in tail:
        raise SystemExit("FAIL: an image no upload takes is tried again forever")
    never = tail[tail.index("} else if (upload_never(*tex)) {"):]
    failed = never[never.index("    } else {"):]
    never = never[:never.index("    } else {")]
    if "tex->uploaded = true;" not in never:
        raise SystemExit("FAIL: an image no upload takes is not marked uploaded")
    for gone in ("tex->uploaded = true", "tex->upload_gen = gen"):
        if gone in failed:
            raise SystemExit("FAIL: a texture whose upload failed is marked done (%s): left empty or stale" % gone)
    for need in ("tex->retry_frame = frame_no + 30;", "tex->gpu_wrote_seen = seen_before;", "told++ < 40",
                 "its upload failed: tried again in 30 frames", "if (from_memory_taken) {",
                 "evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;"):
        if need not in failed:
            raise SystemExit("FAIL: the failed upload's branch has no: " + need)
    epoch = touch.index("    tex->checked_epoch = epoch;\n    // (hookshot) an image whose last upload failed")
    wait = touch.find("if (tex->retry_frame > frame_no) return tex;")
    seen = touch.find("const uint64_t seen_before = tex->gpu_wrote_seen;")
    wrote = touch.index("gpu_wrote_into(*tex)")
    dirty = touch.index("texture_dirty(*tex, since, dirty)")
    if not epoch < wait < seen < wrote < dirty:
        raise SystemExit("FAIL: the wait for a failed upload's retry is not before gpu_wrote_into and the dirty scan")
    if touch.count("retry_frame > frame_no") != 1:
        raise SystemExit("FAIL: more than one wait for a failed upload's retry")
    taken = touch.index("from_memory_taken = true;")
    if touch.index("evicted_from_memory.erase(e);") > taken or taken > touch.index("const bool up_ok = upload("):
        raise SystemExit("FAIL: an evicted texture's memory entry, taken before a failed upload, is not noted to be put back")
    inner = block(src, "bool upload_inner(Texture& tex, const Ranges* dirty) {")
    if "if (upload_never(tex)) return false;" not in inner:
        raise SystemExit("FAIL: upload_inner and the retry do not share the test for an image no upload takes")
    print("PASS: a texture whose upload failed waits 30 frames and is tried again with what it had to take; whatever its kind")


if __name__ == "__main__":
    main()
