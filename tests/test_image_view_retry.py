"""Execute texture-owned view caches with authored Vulkan and cache-commit failures."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    names = ["VkFormat widened(", "VkFormat storage_view_format(", "VkFormat view_format_of(", "VkComponentSwizzle component(",
             "VkImageView make_view(", "template<class Make>\nVkImageView cached_image_view(",
             "VkImageView astc_target_view(", "VkImageView astc_level_view(", "unsigned view_level(",
             "unsigned image_level(", "unsigned view_layer(", "VkImageView texture_view("]
    actual = "\n".join(block(source, n) for n in names)
    upload = block(source, "bool upload_unswizzle_gpu(")
    start = upload.index("        const uint64_t key = mix(0x5B1Cu")
    end = upload.index("        if (!view) return false;", start) + len("        if (!view) return false;")
    actual += "\nbool unswizzle_view(Texture& tex, uint32_t level, VkFormat vf) {\n" + upload[start:end] + "\nreturn true;\n}\n"
    # Only names/numeric tags are modeled; the view/cache code and Vulkan descriptions above are actual source.
    enums = "enum { " + ", ".join(n + "=" + str(1000+i) for i, n in enumerate(sorted(set(re.findall(r"NVN_FORMAT_\w+", actual))))) + " };"
    fixture = (ROOT / "tests/image_view_retry_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* FORMATS */", enums).replace("/* ACTUAL */", actual)
    with tempfile.TemporaryDirectory(prefix="image-view-retry-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-I" + str(a.headers), *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"] if a.sanitize else []),
                        str(cpp), "-o", str(exe)], check=True)
        for route in ["astc-all", "astc-layer", "unswizzle", "sample", "storage", "attachment"]:
            for case in ["healthy", "host-oom", "device-oom", "permanent", "commit-failure", "negative-commit-failure", "independent"]:
                subprocess.run([str(exe), route, case], check=True, timeout=15)
        for case in ["fallback-success", "fallback-oom", "fallback-mixed", "invalid-usage", "invalid-block-mips", "subresources"]:
            subprocess.run([str(exe), "factory", case], check=True, timeout=15)


if __name__ == "__main__":
    main()
