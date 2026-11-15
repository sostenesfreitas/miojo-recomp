"""Execute Off's production dispatch with fake Vulkan: typed output, decode and rejection without GPU work."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.root / 'gpu/vk/vk_temporal.cpp').read_text(encoding='utf-8')
    fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <mutex>
#include "gpu/vk/upscaler_rows.h"
namespace ur = mkwvk::upscaler_rows;
using VkFormat=int; using VkPipeline=int; using VkPipelineLayout=int;
using VkDescriptorType=int; using VkCommandBuffer=int; using VkImageView=int;
inline constexpr int VK_NULL_HANDLE=0, VK_FORMAT_UNDEFINED=0;
constexpr int VK_FORMAT_B10G11R11_UFLOAT_PACK32=122, VK_FORMAT_R16G16B16A16_SFLOAT=97;
constexpr int VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER=1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE=2;
constexpr int VK_IMAGE_LAYOUT_GENERAL=1, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET=1;
constexpr int VK_PIPELINE_BIND_POINT_COMPUTE=1, VK_SHADER_STAGE_COMPUTE_BIT=1;
struct VkDescriptorImageInfo { int sampler=0, imageView=0, imageLayout=0; };
struct VkWriteDescriptorSet {
 int sType=0, dstBinding=0, descriptorCount=0, descriptorType=0;
 const VkDescriptorImageInfo* pImageInfo=nullptr;
};
struct Texture { int format=122; uint32_t width=1000,height=600; };
struct Camera { bool valid=true; uint64_t frame=10; uint32_t width=1000,height=600; };
struct State {
 std::mutex mu; Camera cam;
 int stretch_rgba=0,stretch_rgba_set=0,stretch_rgba_layout=0;
 int out=11,out_layout=12,linear=13;
};
State state;
State& st() { return state; }
int mode=ur::OFF, enabled=-1;
bool tone=true, pipelines=true, rgba_pipeline=true;
int temporal_mode() { return mode==ur::OFF ? ur::GAME : mode; }
bool temporal_off() { return mode==ur::OFF; }
bool scene_tonemap_on() { return tone; }
int env_switch(const char*) { return enabled; }
uint64_t texture_frame_no() { return 10; }
int dispatches=0, bound_pipe=0, bound_layout=0, disturbed=0, creates=0;
uint32_t groups_x=0,groups_y=0;
const uint32_t DLSS_OUT_RGBA_COMP_SPV[]={0x07230203};
bool make_pipeline(const uint32_t* code, size_t, const int*, int, int,
                   int& set,int& layout,int& pipe,const void* specialization=nullptr) {
 assert(code==DLSS_OUT_RGBA_COMP_SPV && specialization==nullptr); // Default kSquare=true: decode once.
 ++creates; if (!rgba_pipeline) return false;
 set=20; layout=21; pipe=22; return true;
}
bool make_pipelines() { return pipelines; }
void note_output(Texture*,uint32_t) {}
void descriptors_disturbed() { ++disturbed; }
void full_barrier(int) {}
template<class... T> void logf(const char*,T...) {}
void vkCmdBindPipeline(int,int,int pipe) { bound_pipe=pipe; }
void vkCmdPushDescriptorSetKHR(int,int,int layout,int,int,const VkWriteDescriptorSet* w) {
 bound_layout=layout; assert(w[0].pImageInfo->sampler==13);
 assert(w[0].pImageInfo->imageView==31 && w[1].pImageInfo->imageView==32);
}
void vkCmdPushConstants(int,int layout,int,int,size_t,const void*) { assert(layout==bound_layout); }
void vkCmdDispatch(int,uint32_t x,uint32_t y,int) { ++dispatches;groups_x=x;groups_y=y; }
/* PRODUCTION */
bool run(Texture& in,Texture& out,bool known=true,uint32_t level=0) {
 // temporal_upscale calls stretch only for the effective GAME mode with Off selected.
 if (temporal_mode()!=ur::GAME || !temporal_off()) return false;
 return stretch(1,123,known,&in,31,&out,32,level);
}
int main() {
 Texture in{97,1000,600},out{97,1000,600};
 // 100% must bypass the network, even with the DLSS opt-in unset or explicitly disabled.
 for(int flag : {-1,0,1}) {
   enabled=flag; assert(run(in,out)); assert(bound_pipe==22 && bound_layout==21);
 }
 assert(creates==1 && groups_x==125 && groups_y==75 && disturbed==3);
 // Spatial scaling, including a mip output; shape-detected networks require the same frame's scene.
 out.width=1500;out.height=900;state.cam={true,10,1000,600};
 assert(run(in,out,false)); assert(groups_x==188 && groups_y==113);
 out.width=3000;out.height=1800; assert(run(in,out,true,1));
 int before=dispatches;
 state.cam={false,10,1000,600}; assert(!run(in,out,false));
 state.cam={true,8,1000,600}; assert(!run(in,out,false));
 state.cam={true,10,999,600}; assert(!run(in,out,false));
 out.width=1000;out.height=600; assert(!run(in,out,false)); // Unknown same-size compute is not a network.
 out.width=999; assert(!run(in,out));
 out.width=1000;out.format=122; assert(!run(in,out)); // Never mix packed and RGBA.
 in.format=122;out.format=97; assert(!run(in,out));
 in.format=97;out.format=97;tone=false; assert(!run(in,out));tone=true;
 mode=ur::GAME; assert(!run(in,out));
 mode=ur::FSR3; assert(!run(in,out));
 mode=ur::OFF;in.format=44;out.format=44; assert(!run(in,out));
 assert(dispatches==before);
 in.format=out.format=97;state.stretch_rgba=0;rgba_pipeline=false;
 assert(!run(in,out));assert(dispatches==before);rgba_pipeline=true;
 assert(run(in,out)); // Failed allocation can retry.
 in.format=out.format=122;assert(run(in,out));assert(bound_pipe==11 && bound_layout==12);
}
'''
    signatures = ['bool temporal_scene_format(', 'bool temporal_colour_pair(', 'bool stretch(']
    # On the unfixed source this helper is absent and stretch rejects RGBA before dispatch.
    # Compile that real implementation too, so the baseline fails at the native-path assertion.
    if 'bool make_rgba_stretch(' in source:
        signatures.insert(2, 'bool make_rgba_stretch(')
    production = '\n'.join(function(source, signature) for signature in signatures)
    assert 'if (mode == ur::GAME) return temporal_off() && stretch(' in function(source, 'bool temporal_upscale(')
    fixture = fixture.replace('/* PRODUCTION */', production)
    with tempfile.TemporaryDirectory(prefix='psycho-off-') as folder:
        cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(args.root), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PsychoV35 Off: native/scaled dispatch, decoding pipeline, format/camera guards and failure recovery passed')


if __name__ == '__main__':
    main()
