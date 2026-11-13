"""Native FSR constant bytes stay owned by their recording, without a game or GPU."""
RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

import argparse
import subprocess
import tempfile
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", required=True)
    args = p.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    internal = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/fsr_uniform_ownership_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* RES */", block(draw, "struct RecordingRes {") + ";")
    fixture = fixture.replace("/* BUFFER_TYPES */", block(internal, "struct BufferRef {") + ";\n" + block(internal, "struct UniformUpload {") + ";")
    actual = "\n".join(block(draw, m) for m in ["bool stream_insert(", "bool stream_next(", "bool stream_reserve(", "uint64_t stream_alloc(", "UniformUpload uniform_upload("])
    fixture = fixture.replace("/* STREAM */", actual).replace("/* BEGIN */", block(draw, "void draw_begin_recording("))
    fixture = fixture.replace("/* RECORDING_WAIT */", block(replay, "void recording_wait(")).replace("/* RECORDING_BEGIN */", block(replay, "Recording& begin_recording("))
    with tempfile.TemporaryDirectory(prefix="fsr-uniforms-") as tmp:
        for name in ["vk_fsr3.cpp", "vk_fsr3fi.cpp"]:
            source = (ROOT / "gpu/vk" / name).read_text(encoding="utf-8")
            fi = name == "vk_fsr3fi.cpp"
            names = ["Constants", "PyramidConstants", "OfConstants", "OfSpdConstants"] if fi else ["Constants", "SpdConstants"]
            types = "\n".join(block(source, "struct " + n + " {") + ";" for n in names) + "\n" + block(source, "struct Image {") + ";"
            fields = "slot_fi=0,slot_pyramid=0,slot_of=0,slot_of_spd=0" if fi else "slot_fsr=0,slot_spd=0"
            state = "struct { VkDeviceSize " + fields + ",frame_stride=0;bool lut_uploaded=true,first_evaluation=true,first_frame=true;uint64_t sequence=0;uint32_t parity=0;Image made[1];} g;"
            if fi:
                types += "\nconstexpr uint32_t kOfLevels = 7;"
                inputs = "Constants k{};PyramidConstants game_pyramid{},frame_pyramid{};OfConstants of{};OfSpdConstants of_spd{};std::memset(&k,seed,sizeof k);std::memset(&game_pyramid,seed+1,sizeof game_pyramid);std::memset(&frame_pyramid,seed+2,sizeof frame_pyramid);std::memset(&of,seed+3,sizeof of);of.pyramid_level=0;std::memset(&of_spd,seed+4,sizeof of_spd);"
            else:
                inputs = "Constants k{};SpdConstants spd{};std::memset(&k,seed,sizeof k);std::memset(&spd,seed+1,sizeof spd);"
            reserve = source[source.index("    const UniformUpload uniforms = uniform_upload(g.frame_stride);"):]
            reserve = reserve[:reserve.index("return false;") + len("return false;")].replace("return false;", "return {};")
            start = source.index("    // This evaluation owns its constants")
            next_section = "    const Resource cb_fi{" if fi else "    const uint32_t p = g.parity"
            end = source.index(next_section, start)
            upload = source[start:end]
            cb = "cb_fi" if fi else "cb_fsr"
            begin = source.index("    const Resource " + cb + "{")
            descriptor = source[begin:source.index(";", begin) + 1]
            types += "\n" + block(source, "struct Resource {") + ";"
            extra = ""
            pairs = [("cb_game_pyramid", "game_pyramid"), ("cb_frame_pyramid", "frame_pyramid"), ("cb_of_spd", "of_spd")] if fi else [("cb_spd", "spd")]
            for binding, expected in pairs:
                at = source.index("    const Resource " + binding + "{")
                extra += source[at:source.index(";", at) + 1]
                extra += "pending.push_back({{" + binding + ".buffer," + binding + ".offset," + binding + ".size,0},{reinterpret_cast<uint8_t*>(&" + expected + "),reinterpret_cast<uint8_t*>(&" + expected + ")+sizeof " + expected + "}});"
            if fi:
                extra += block(source, "    const auto cb_of =") + ";"
                extra += "for(uint32_t level=0;level<=kOfLevels;level++){OfConstants expected=of;if(level)expected.pyramid_level=level-1;auto r=cb_of(level);pending.push_back({{r.buffer,r.offset,r.size,0},{reinterpret_cast<uint8_t*>(&expected),reinterpret_cast<uint8_t*>(&expected)+sizeof expected}});}"
            entry = "bool generate(" if fi else "bool upscale("
            a = source.index(entry)
            b = source.index("if (!uniforms.binding) return false;", a) + len("if (!uniforms.binding) return false;")
            admission = source[a:b].replace("bool generate(", "bool admitted(").replace("bool upscale(", "bool admitted(").replace("VkCommandBuffer cmd", "[[maybe_unused]] VkCommandBuffer cmd") + "\n    return true;\n}\n"
            stubs = "struct Inputs {VkImageView color=reinterpret_cast<VkImageView>(uintptr_t(1)),depth=color,motion=color;VkImage color_image=reinterpret_cast<VkImage>(uintptr_t(1));VkFormat color_format=VK_FORMAT_R8G8B8A8_UNORM;uint32_t render_width=1280,render_height=720,upscale_width=1920,upscale_height=1080,width=1920,height=1080;struct {float device_to_view_depth[2]={1,1};} camera;};void free_dead(){}template<class...T>bool prepare(T...){return true;}[[maybe_unused]] uint32_t frames_wanted(){return 1;}"
            helper = stubs + admission + "BufferRef upload(unsigned seed) {" + reserve + "\n" + inputs + "\n" + upload + "\n" + descriptor + "\n" + extra + "\n" + "return {" + cb + ".buffer," + cb + ".offset," + cb + ".size,0};}"
            code = fixture.replace("/* BACKEND */", types + "\n" + state + "\n" + block(source, "void make_uniform_layout()") + "\n" + helper)
            cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + ".exe")
            cpp.write_text(code, encoding="utf-8")
            subprocess.run([args.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-pthread", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-I" + args.headers, str(cpp), "-o", str(exe)], check=True)
            for cache in ["coherent", "noncoherent"]:
                for case in ["queued", "growth", "reuse", "workers", "failure", "growth-off", "empty", "no-recording", "initial-missing", "admission-failure", "fence-reuse", "batch-reuse"]:
                    subprocess.run([str(exe), case, cache], check=True, timeout=20)
                    print(name, cache, case, "passed", flush=True)


if __name__ == "__main__":
    main()
