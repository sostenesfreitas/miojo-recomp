"""Execute sampler creation and every handle-cache layer with authored failures."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    nvn = (a.source / "gpu/render/rx_nvn.h").read_text(encoding="utf-8")
    actual = "\n".join(block(source, marker) for marker in [
        "VkSamplerAddressMode wrap(", "uint32_t aniso_override() {", "VkCompareOp compare_op(",
        "VkSampler sampler_make(int id, bool compare) {", "VkSampler sampler_for(int id, bool compare) {"])
    # Keep standard-container behavior, replacing only the allocator to target an individual cache.
    for old, new in [
        ("std::unordered_map<int, std::pair<uint32_t, nvn_samp_par>> seen", "FaultMap<int, std::pair<uint32_t, nvn_samp_par>, Seen> seen"),
        ("std::unordered_map<uint64_t, VkSampler> mine", "FaultMap<uint64_t, VkSampler, Mine> mine"),
        ("std::unordered_set<uint64_t> failed", "FaultSet<uint64_t, Failure> failed"),
    ]:
        assert actual.count(old) == 1, old
        actual = actual.replace(old, new)
    start = nvn.rfind("typedef struct {", 0, nvn.index("} nvn_samp_par;"))
    params = nvn[start:nvn.index("} nvn_samp_par;", start) + len("} nvn_samp_par;")]
    symbols = sorted(set(re.findall(r"\bNVN_[A-Z0-9_]+", actual)))
    enums = "enum { " + ", ".join(name + "=" + str(i + 1) for i, name in enumerate(symbols)) + " };"
    fixture = (ROOT / "tests/sampler_admission_test.cpp").read_text(encoding="utf-8")
    descriptors = block(source, "struct Pushed {") + ";\n" + "\n".join(block(source, marker) for marker in [
        "void write_words(", "void words_append(", "int words_same_store(", "bool bind_descriptors("])
    fixture = fixture.replace("/* DESCRIPTORS */", descriptors)
    fixture = fixture.replace("/* PARAMS */", params).replace("/* ENUMS */", enums).replace("/* ACTUAL */", actual)
    cases = ["healthy", "host-oom", "device-oom", "unknown", "error-output", "generation-retry", "thread-retry",
             "independent", "valid-after-failure", "owner-commit", "seen-commit", "mine-commit", "failure-commit",
             "registration", "shared-thread", "concurrent", "nearest", "linear", "compare", "compare-explicit",
             "compare-pcf-off", "custom-border", "standard-border", "reduction", "compare-reduction", "anisotropy",
             "anisotropy-row", "anisotropy-game", "anisotropy-env-off", "anisotropy-change",
             "anisotropy-no-mips", "anisotropy-nearest-mip",
             "upscale-bias", "upscale-bias-plain", "upscale-bias-change",
             "unknown-id", "force-linear", "wrap", "lod-clamp", "custom-reduction",
             "descriptor-push", "descriptor-pool", "descriptor-compute", "descriptor-old", "descriptor-whole"]
    with tempfile.TemporaryDirectory(prefix="sampler-admission-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-pthread", "-I" + str(a.headers),
                        *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"] if a.sanitize else []),
                        str(cpp), "-o", str(exe)], check=True)
        for route in ["id", "make", "cache-off"]:
            for case in cases:
                subprocess.run([str(exe), route, case], check=True, timeout=20)
    print(f"{len(cases) * 3} sampler cache/admission/policy cases passed")


if __name__ == "__main__":
    main()
