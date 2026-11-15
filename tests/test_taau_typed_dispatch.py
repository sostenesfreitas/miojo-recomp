"""Run the production typed-upscale dispatch with Vulkan spies: format changes must reset and retire history."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}', '--headers', '{vulkan_headers}']
ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    start = text.rindex(signature) # Skip the forward declaration before upscale_stub.
    end = text.index('{', start)+1
    depth = 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--headers', required=True)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    production = block((args.source_root/'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8'), 'static bool typed_upscale(VkCommandBuffer cmd,')
    fixture = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gpu/vk/shaders/taau_upscale_comp_spv.h"
#include "gpu/vk/shaders/taau_upscale_rgba_comp_spv.h"
using VmaAllocation=void*;
struct Texture { uint32_t width=128,height=64; VkFormat format=VK_FORMAT_B10G11R11_UFLOAT_PACK32; };
struct { bool push_descriptor=true; VkDevice device=nullptr; } dev;
template<class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
uintptr_t next_handle=100;
uint64_t frame=10;
bool tone=true;
unsigned shader_creates=0,images=0,retired=0,dispatches=0,disturbed=0;
uint32_t push_bits=0;
VkFormat history_format=VK_FORMAT_UNDEFINED;
VkPipeline bound=nullptr;
bool scene_tonemap_on(){return tone;}
bool env_on(const char* name){return !strcmp(name,"RX_VK_TAAU_CLAMP");}
uint64_t texture_frame_no(){return frame;}
struct IntelDriverGate {};
VkResult vkCreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout* p){*p=handle<VkDescriptorSetLayout>(++next_handle);return VK_SUCCESS;}
VkResult vkCreatePipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout* p){*p=handle<VkPipelineLayout>(++next_handle);return VK_SUCCESS;}
VkResult guarded_shader_module(const VkShaderModuleCreateInfo* info,VkShaderModule* p){
 ++shader_creates;assert(info->pCode==(shader_creates==1?TAAU_UPSCALE_COMP_SPV:TAAU_UPSCALE_RGBA_COMP_SPV));
 *p=handle<VkShaderModule>(++next_handle);return VK_SUCCESS;
}
VkResult guarded_compute(VkPipelineCache,const VkComputePipelineCreateInfo*,VkPipeline* p){*p=handle<VkPipeline>(++next_handle);return VK_SUCCESS;}
void vkDestroyShaderModule(VkDevice,VkShaderModule,const VkAllocationCallbacks*){}
VkResult vkCreateSampler(VkDevice,const VkSamplerCreateInfo*,const VkAllocationCallbacks*,VkSampler* p){*p=handle<VkSampler>(++next_handle);return VK_SUCCESS;}
void vkDestroyImageView(VkDevice,VkImageView,const VkAllocationCallbacks*){}
void destroy_image(VkImage,VmaAllocation){++retired;}
bool create_image(const VkImageCreateInfo& info,VkImage& image,VmaAllocation& mem){
 ++images;history_format=info.format;assert(info.extent.width==128 && info.extent.height==64);
 image=handle<VkImage>(++next_handle);mem=handle<void*>(++next_handle);return true;
}
VkResult vkCreateImageView(VkDevice,const VkImageViewCreateInfo* info,const VkAllocationCallbacks*,VkImageView* p){
 assert(info->format==history_format);*p=handle<VkImageView>(++next_handle);return VK_SUCCESS;
}
void vkCmdPipelineBarrier2(VkCommandBuffer,const VkDependencyInfo*){}
void vkCmdBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline pipe){bound=pipe;}
void VKAPI_CALL push(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t count,const VkWriteDescriptorSet* w){
 assert(count==3 && w[0].pImageInfo->imageView==handle<VkImageView>(31) && w[1].pImageInfo->imageView==handle<VkImageView>(32));
 assert(w[2].pImageInfo->imageView!=nullptr);
}
PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR=push;
void vkCmdPushConstants(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t bytes,const void* p){assert(bytes==12);push_bits=static_cast<const uint32_t*>(p)[2];}
void vkCmdDispatch(VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z){assert(x==16 && y==8 && z==1);++dispatches;}
void descriptors_disturbed(){++disturbed;}
template<class... T> void logf(const char*,T...){}
template<class... T> void probe_image(T...){}
/* PRODUCTION */
int main(){
 Texture out;
 auto run=[&](bool temporal=true){return typed_upscale(nullptr,handle<VkImageView>(31),handle<VkImageView>(32),&out,0,1,"test",temporal);};
 assert(run() && push_bits==2 && images==1);VkPipeline packed=bound;
 ++frame;assert(run() && push_bits==3 && images==1);
 out.format=VK_FORMAT_R16G16B16A16_SFLOAT;++frame;
 assert(run() && push_bits==2 && images==2 && shader_creates==2 && bound!=packed);
 VkPipeline rgba=bound;++frame;assert(run() && push_bits==3 && bound==rgba);
 out.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;++frame;
 assert(run() && push_bits==2 && images==3 && shader_creates==2 && bound==packed && retired==0);
 frame=200;assert(run() && push_bits==2 && retired==2); // In-flight images survived 120 presents.
 assert(run(false) && push_bits==0 && images==4); // Spatial/movie history remains separate.
 unsigned before=dispatches;out.format=VK_FORMAT_R16G16B16A16_SFLOAT;
 assert(!run(false));tone=false;assert(!run());tone=true;
 out.format=VK_FORMAT_R8G8B8A8_UNORM;assert(!run());
 assert(dispatches==before && disturbed==dispatches && shader_creates==2);
}
'''.replace('/* PRODUCTION */', production)
    with tempfile.TemporaryDirectory(prefix='taau-dispatch-') as folder:
        cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-I'+str(ROOT), '-I'+args.headers,
                        str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('TAAU dispatch: typed pipelines, format resets, history separation and deferred retirement pass')


if __name__ == '__main__':
    main()
