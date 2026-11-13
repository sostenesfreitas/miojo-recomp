"""A BC3 texture whose upload failed is tried again, not drawn as it was left.

Decoded ASTC is re-encoded to BC3 on the GPU, which needs staging memory for the ASTC blocks and the BC3 scratch. With the
VRAM full (a 4 GB card after an alt-tab: the textures evicted and made again) those could not be had, the upload failed,
and the texture was marked uploaded anyway: its uninitialized blocks were drawn as coloured noise and never made again
unless the game rewrote its memory. Now it stays not uploaded and is tried again 30 frames on, and the log says why.

python -I -B tests/test_bc_upload_retry.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def main():
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_texture.h").read_text(encoding="utf-8")
    if "uint64_t retry_frame = 0;" not in header:
        raise SystemExit("a texture has no frame to try a failed BC3 upload again from")
    failed = tex[tex.index("} else if (upload_never(*tex)) {"):]
    failed = failed[failed.index("} else {"):failed.index("    return tex;\n}")]
    if "tex->uploaded = true" in failed or "tex->retry_frame = frame_no + 30;" not in failed or "BC3 texture" not in failed:
        raise SystemExit("a BC3 texture whose upload failed is marked uploaded (drawn as noise, never made again)")
    if "if (tex->retry_frame > frame_no) return tex;" not in tex:
        raise SystemExit("a failed BC3 upload is tried again every submission (it stages the whole ASTC source)")
    upload = tex[tex.index("bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {"):]
    upload = upload[:upload.index("\n}\n")]
    if upload.count("return false;") != upload.count("astc_fail = \"") or "no memory for the BC3 scratch" not in upload:
        raise SystemExit("a failed ASTC upload does not say why")
    # (the scratch's output buffer too: only in VRAM it could not be made with the VRAM full, and every BC3 texture made
    # again then failed, "no memory for the BC3 scratch" in the log)
    scratch = tex[tex.index("bool bc_scratch_make() {"):]
    scratch = scratch[:scratch.index("\n}\n")]
    if "create_buffer(out, out_usage, false, bc_out, bc_out_alloc, nullptr, !pool)" not in scratch:
        raise SystemExit("the BC3 scratch's output buffer cannot go to system memory when the VRAM is full")
    print("failed BC3 uploads are tried again, with their cause logged")


if __name__ == "__main__":
    main()
