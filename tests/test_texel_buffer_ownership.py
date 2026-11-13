"""Run formatted views with real binding resolution and completed import destruction."""
import argparse
from pathlib import Path
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ['--cxx', '{cxx}', '--headers', '{vulkan_headers}']

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    texture = (a.source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    memory = (a.source / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    start = texture.index("using TexelViewKey =")
    end = texture.index("} // namespace", start)
    state = texture[start:end]
    # Use std::map's real allocation/commit behavior with a throwing allocator. No production branch is replaced.
    assert state.count("std::map<") == 2
    state = state.replace("std::map<", "FaultMap<")
    eviction = block(memory, "void imports_give_back()")
    eviction = eviction[:eviction.index("    // (hookshot) Off by default")] + "}\n"
    actual = "\n".join([
        state,
        block(texture, "void release_texel_buffer_views("),
        block(memory, "void destroy_import_buffer("),
        block(memory, "BufferRef gpu_buffer("),
        block(texture, "VkBufferView texel_buffer_view("),
        block(memory, "void reap_mirrors("),
        eviction,
    ])
    # Rollback, prefetch drops, a trim's rollback (mirrors_trim) and both published-import reapers must share the same
    # owner-release point.
    assert memory.count("vkDestroyBuffer(") == 1
    assert memory.count("destroy_import_buffer(") == 10
    fixture = (ROOT / "tests/texel_buffer_ownership_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* DEAD IMPORT */", block(memory, "struct DeadImport {") + ";")
    fixture = fixture.replace("/* ACTUAL */", actual)
    cases = ["healthy", "remap", "offset", "format", "range", "liveness", "late-map",
             "host-oom", "device-oom", "other-error", "registry-commit", "view-commit",
             "cached-after-failure", "independent", "retirement", "eviction", "reuse-handle",
             "storage", "alignment", "zero-limit", "clamped-range", "invalid", "concurrent"]
    with tempfile.TemporaryDirectory(prefix="texel-buffer-ownership-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-pthread", "-I" + str(a.headers),
                        *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"] if a.sanitize else []),
                        str(cpp), "-o", str(exe)], check=True)
        for case in cases:
            subprocess.run([str(exe), case], check=True, timeout=20)
    print(f"{len(cases)} binding/view/retirement cases passed")


if __name__ == "__main__":
    main()
