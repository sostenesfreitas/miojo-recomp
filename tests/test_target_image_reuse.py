"""A released render target's image goes to the pool as it is evicted, when the GPU is done with it, so a target of the
same create info made in the next frame takes it instead of making a new image.

The present's texture_frame runs between two game frames. An image pooled there, in the dead list, is pooled at the next
present, after the next frame has already been recorded: that frame's target of its shape missed it, made a new image, and
the old one was destroyed unused 120 presents later. OoT's transient targets come back for a few frames before they go
(issue 2180's in-game A/B: 65% more targets let go, 83% more images made new). This runs the pool's and the eviction's
real code from vk_texture.cpp with a stubbed GPU.

python -I -B tests/test_target_image_reuse.py [--cxx g++] --headers <Vulkan-Headers/include>
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def blocks(text, marker):
    found = []
    start = 0
    while True:
        try:
            start = text.index(marker, start)
        except ValueError:
            return found
        end = text.index("{", start) + 1
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        found.append(text[start:end])
        start = end


def check_source(tex):
    frame = block(tex, "void texture_frame(uint64_t completed)")
    evict = next((b for b in blocks(frame, "for (auto& [last, tex] : order) {") if "evict.push_back({tex," in b), "")
    if not evict:
        raise SystemExit("texture_frame no longer has the loop that chooses the targets to destroy")
    if "if (tex->last_value > completed) continue;" not in evict:
        raise SystemExit("an in-flight texture can be evicted: its image would be pooled while the GPU reads it")
    if "destroy_later(*tex);" not in frame:
        raise SystemExit("the eviction no longer pools an image the GPU is done with at once: the next frame makes a new one")
    if "void destroy_later(Texture& tex)" not in tex:
        raise SystemExit("destroy_later no longer pools at once: an image would wait for the next present")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    tex = (a.source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    check_source(tex)
    start = tex.index("struct Pooled {")
    end = tex.index("bool pool_pressed;", start) + len("bool pool_pressed;")
    pool = tex[start:end]
    actual = "\n".join([
        block(tex, "uint64_t mix("),
        pool,
        block(tex, "uint64_t pool_cap()"),
        block(tex, "uint64_t pool_key_of("),
        block(tex, "bool pool_take("),
        block(tex, "void pool_destroy("),
        block(tex, "void pool_put("),
        block(tex, "size_t pool_trim()"),
        block(tex, "void destroy_later("),
    ])
    fixture = (ROOT / "tests/test_target_image_reuse.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* DEAD */", block(tex, "struct Dead {") + ";").replace("/* ACTUAL */", actual)
    with tempfile.TemporaryDirectory(prefix="target-image-reuse-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-D_CRT_SECURE_NO_WARNINGS",
                        "-pthread", "-I" + str(a.headers),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("a released target's image is the pool's for the next frame: reused, not made")


if __name__ == "__main__":
    main()
