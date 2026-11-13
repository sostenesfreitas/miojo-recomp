"""The production shadow guard runs once after its draw, never during arming."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
p = argparse.ArgumentParser()
p.add_argument('--cxx', required=True)
a = p.parse_args()
source = (Path(__file__).resolve().parents[1] / 'gpu/vk/vk_draw.cpp').read_text()
stabilize = source[source.index('void mask_stabilize('):source.index('void draw(VkCommandBuffer')]
assert stabilize.index('end_pass(cmd)') < stabilize.index('vkCmdPipelineBarrier2(')
assert stabilize.index('end_pass(cmd)') < stabilize.index('vkCmdDispatch(')
start = source.index('    struct MaskGuard {')
end = source.index('    if (!record_progs()', start)
actual = source[start:end]
fixture = r'''
#include <cstdint>
#include <cassert>
#include <vector>
#include <atomic>
using VkCommandBuffer=uint64_t;
constexpr VkCommandBuffer VK_NULL_HANDLE=0;
struct Texture {};
struct {VkCommandBuffer cmd=0;uint64_t buffer=0;} draw_predicate_binding;
void logf(const char*,...) {}
float weight=.85f;
float mask_weight(){return weight;}
std::vector<int> events;
void mask_stabilize(VkCommandBuffer cmd,Texture* tex,uint32_t level,uint32_t layer,float w){
 assert(cmd==1&&tex&&level==2&&layer==3&&w==weight);events.push_back(2);
}
void production_draw(uint64_t key,int targets,bool enabled){
 struct {uint64_t key;}p{key};Texture texture;
 struct {bool render_enabled;}s{enabled};
 struct {int n;Texture* color[1];uint32_t color_level[1],color_layer[1];}at{targets,{&texture},{2},{3}};
 VkCommandBuffer cmd=1;
 @@ACTUAL@@
 assert(events.empty());events.push_back(draw_predicate_binding.buffer?3:1);
}
void check(uint64_t key,int targets,float w,bool enabled,bool predicate,std::vector<int> expected){
 events.clear();weight=w;draw_predicate_binding={1,predicate?uint64_t(7):uint64_t(0)};
 if(enabled||predicate)production_draw(key,targets,enabled);
 assert(events==expected);
}
int main(){
 constexpr uint64_t mask=0x3427a1dbefd5e02cull;
 check(mask,1,.85f,true,false,{1,2});
 check(mask,1,0,true,false,{1});
 check(mask,0,.85f,true,false,{1});
 check(0,1,.85f,true,false,{1});
 check(mask,1,.85f,false,false,{});

}

'''
with tempfile.TemporaryDirectory() as t:
    cpp, exe = Path(t)/'test.cpp', Path(t)/'test'
    cpp.write_text(fixture.replace('@@ACTUAL@@', actual))
    subprocess.run([a.cxx, '-std=c++20', '-O1', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Shadow stabilization runs once after its draw: PASS')
