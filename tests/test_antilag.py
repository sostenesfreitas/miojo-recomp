"""AMD Anti-Lag 2 (gpu/vk/vk_antilag.cpp): compiles its actual frame calls against the Vulkan headers with a recording
vkAntiLagUpdateAMD, and checks that the device and the present path call them, with frame generation's display thread
too (each set keeps its game frame, and only that frame's present gives its present stage, as NVIDIA Reflex's markers).
No GPU or driver required.

python -I -B tests/test_antilag.py --headers <Vulkan-Headers/include> [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--headers", type=Path, required=True)
p.add_argument("--cxx", default="g++")
a = p.parse_args()
source = (ROOT / "gpu" / "vk" / "vk_antilag.cpp").read_text(encoding="utf-8")


def present_image_block(text):
    return block(text, "void present_image(")


def block(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


# the wiring: the device asks for the extension and its feature, the present gives the present stage before
# vkQueuePresentKHR, and the input stage comes after the present (with Reflex's sleep)
main = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
for call in ("antilag_device_extensions(want, exts);", "antilag_device_features(tail);", "antilag_device_created();"):
    if call not in main:
        raise SystemExit("vk_main.cpp no longer calls " + call)
if main.index("antilag_device_features(tail);") > main.index("vkGetPhysicalDeviceFeatures2(dev.physical, &f);"):
    raise SystemExit("vk_main.cpp: Anti-Lag 2's feature joins the chain after the features were queried")
replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
# (present ahead, #1720: present_image's present is its end, present_out, which present_commit also runs)
present = block(replay, "static bool present_out(uint32_t index, std::unique_lock<std::mutex>& qk) {")
if "antilag_present();" not in present or present.index("antilag_present();") > present.index("vkQueuePresentKHR("):
    raise SystemExit("present_out: Anti-Lag 2's present stage is not given before vkQueuePresentKHR")
if "reflex_sleep(); antilag_input();" not in block(replay, "void do_present("):
    raise SystemExit("do_present: Anti-Lag 2's input stage is not given after the present")
# present ahead: the deferred present keeps the frame it shows until its commit, as Reflex's
if "present_defer_into->antilag_frame = antilag_present_frame;" not in present_image_block(replay):
    raise SystemExit("present_image: a deferred present does not keep the frame it shows")
commit = block(replay, "bool present_commit(")
if "antilag_present_frame = p.antilag_frame;" not in commit or "antilag_present_frame = al_was;" not in commit:
    raise SystemExit("present_commit: the deferred present's frame is not the one its present stage gives")
# frame generation: the set keeps its game frame, and only the set's game frame gives its present stage (both paths)
framegen = (ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8")
if "set->antilag_id = antilag_frame();" not in framegen:
    raise SystemExit("framegen_record: the set does not keep its game frame")
pick = "antilag_present_frame = (timed ? j.set->game[k] : k == j.set->made) ? j.set->antilag_id : 0;"
if block(framegen, "void display_main()").count(pick) != 2:
    raise SystemExit("display_main: a set's presents (direct and present ahead) do not say which frame they show")

functions = "\n".join(block(source, m) + (";" if m.startswith("struct") else "") for m in (
    "struct AntiLag {", "AntiLag& al()", "void update(", "bool on()", "void stage(", "uint64_t antilag_frame()",
    "void antilag_present()", "void antilag_input()"))
fixture = r'''
#include <vulkan/vulkan_core.h>
#include <cstdio>
#include <mutex>
#include <vector>
struct {VkDevice device=VK_NULL_HANDLE;} dev;
int want=1;
constexpr uint64_t kAntiLagThisFrame=~0ull;
thread_local uint64_t antilag_present_frame=kAntiLagThisFrame;
bool wanted(){return want>0;}
template<class... T> void logf(const char*,T...){}
struct Call{VkAntiLagModeAMD mode;int stage;uint64_t frame;};
std::vector<Call> calls;
void VKAPI_CALL record(VkDevice,const VkAntiLagDataAMD* d){
    calls.push_back({d->mode,d->pPresentationInfo?(int)d->pPresentationInfo->stage:-1,
                     d->pPresentationInfo?d->pPresentationInfo->frameIndex:0});
}
''' + functions + r'''
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(){
    // not available: nothing is ever called
    antilag_input();antilag_present();CHECK(calls.empty()&&antilag_frame()==0);
    al().ready=true;al().update=record;
    // before the first input stage: no frame, and a present gives nothing
    CHECK(antilag_frame()==0);antilag_present();CHECK(calls.empty());
    // a frame without frame generation: its input stage, then its present stage, with one index, mode On
    antilag_input();uint64_t f=antilag_frame();CHECK(f!=0);antilag_present();
    CHECK(calls.size()==2&&calls[0].stage==VK_ANTI_LAG_STAGE_INPUT_AMD&&calls[1].stage==VK_ANTI_LAG_STAGE_PRESENT_AMD);
    CHECK(calls[0].frame==f&&calls[1].frame==f&&calls[0].mode==VK_ANTI_LAG_MODE_ON_AMD&&calls[1].mode==VK_ANTI_LAG_MODE_ON_AMD);
    // frame generation: the set's game frame is shown after the game's next input stage, with its own frame's index
    calls.clear();antilag_input();const uint64_t g=antilag_frame();antilag_input();
    antilag_present_frame=0;antilag_present();CHECK(calls.size()==2);   // (a made frame before it: nothing)
    antilag_present_frame=g;antilag_present();
    CHECK(calls.size()==3&&calls[1].frame==g+1&&calls[2].stage==VK_ANTI_LAG_STAGE_PRESENT_AMD&&calls[2].frame==g);
    antilag_present_frame=kAntiLagThisFrame;
    // the row off: Off once, then nothing, and no frame for a set to keep
    calls.clear();want=0;antilag_input();antilag_present();antilag_input();
    CHECK(calls.size()==1&&calls[0].mode==VK_ANTI_LAG_MODE_OFF_AMD&&calls[0].stage==-1&&antilag_frame()==0);
    // on again: a set made before gives nothing; the next frame's stages go out
    calls.clear();want=1;antilag_present_frame=g+1;antilag_present();CHECK(calls.empty());
    antilag_present_frame=kAntiLagThisFrame;antilag_present();CHECK(calls.empty());   // (no input stage since)
    antilag_input();f=antilag_frame();antilag_present();
    CHECK(calls.size()==2&&calls[1].stage==VK_ANTI_LAG_STAGE_PRESENT_AMD&&calls[1].frame==f&&f>g+1);
    std::puts("PASS: Anti-Lag 2's stages with one index a frame, frame generation's game frames shown later too; Off told once");
}
'''
with tempfile.TemporaryDirectory(prefix="antilag-") as tmp:
    cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    "-pthread", "-I" + str(a.headers), str(cpp), "-o", str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
