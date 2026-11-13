"""Execute production scaler/init functions with authored API spies, without a game or Vulkan device.

The direct-present view's forwarding/age bookkeeping is a separate, explicitly source-based wiring check.
--scale-source can supply an original vk_framegen.cpp for the runtime negative control.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    if source.count(marker) != 1:
        raise ValueError("Expected exactly one " + marker)
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--scale-source", type=Path)
    a = p.parse_args()
    source = (ROOT / "gpu/vk/vk_framegen.cpp").read_text(encoding="utf-8")
    scale_source = a.scale_source.read_text(encoding="utf-8") if a.scale_source else source
    replay = block((ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8"), "void do_present(")
    # Wiring check only: resource/dispatch behavior is exercised by the real functions below.
    capture = replay.index("direct_view = texture_view(")
    age = replay.index("texture_frame(completed());")
    assert replay.index("std::lock_guard lk(replay_mu)") < capture < age
    assert capture < replay.index("tex->last_frame = texture_frame_no();") < age
    assert capture < replay.index("tex->last_value = std::max(tex->last_value, value);") < age
    assert "present_image(src, sw, sh, crop, direct_view);" in replay
    functions = (block(source, "struct ScaleState") + " scale;\n" + block(source, "struct WideSides") + ";\nstd::atomic<WideSides> wide_now;\n" +
                 "thread_local bool wide_cover_in_frame = false;\n" +
                 block(source, "float wide_cover_held(") + "\n" + block(source, "bool init_scale(") + "\n" +
                 block(source, "bool framegen_wanted(") + "\n" + block(source, "bool framegen_on(") + "\n" +
                 block(scale_source, "VkImage framegen_scale("))
    template = (ROOT / "tests/upscale_independent_test.cpp").read_text(encoding="utf-8")
    assert template.count("// PRODUCTION_FUNCTIONS") == 1
    fixture = template.replace("// PRODUCTION_FUNCTIONS", block(source, "void drop_pipe(") + "\n" + functions)
    with tempfile.TemporaryDirectory(prefix="upscale-independent-") as tmp:
        owned = Path(tmp).resolve()
        assert owned.parent == Path(tempfile.gettempdir()).resolve() and owned.name.startswith("upscale-independent-")
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(cpp), "-o", str(exe)], check=True)
        for allow in ("0", "1"):
            env = os.environ.copy()
            env["RX_VK_FRAMEGEN"] = allow
            if allow == "0":
                env["RX_VK_UPSCALE_LOG"] = "1"
            else:
                env.pop("RX_VK_UPSCALE_LOG", None)
            subprocess.run([str(exe)], env=env, check=True)


if __name__ == "__main__":
    main()
