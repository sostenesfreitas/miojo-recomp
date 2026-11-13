"""Extract the real state-list reader; reject bad counts but keep valid records.

Synthetic records only. No Vulkan, game, or ETW API. Run with --source ROOT.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT=Path(__file__).resolve().parents[1]

def block(text, marker):
    start=text.index(marker);brace=text.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT)
    parser.add_argument('--cxx',default='clang++')
    args=parser.parse_args()
    text=(args.source/'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    targets=block(text,'bool targets_ok(')+'\n'+block(text,'bool formats_ok(')
    # (version 2 records and a version 1 mesh record's numbering: tests/test_pipeline_state_portable.py. These records draw no mesh)
    actual='\n'.join((block(text,'constexpr char kStateMagic[8]')+';',block(text,'constexpr char kStateMagic2[8]')+';',
                       block(text,'struct StateRec {')+';',block(text,'struct StateRec2 {')+';',
                       'static bool state_v1_portable(StateRec& r){return !r.mesh;}',
                       'static bool state_v2_resolve(const StateRec2&,StateRec&){return false;}',
                       block(text,'std::vector<StateRec> state_list_read(')))
    fixture=r'''
#define _CRT_SECURE_NO_WARNINGS 1
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
constexpr uint32_t VK_PRIMITIVE_TOPOLOGY_PATCH_LIST = 10;   // (vulkan_core.h)
// (a device that draws to RGBA8 (37) and has D32 (126) as depth: vkGetPhysicalDeviceFormatProperties's answer)
enum VkFormat : uint32_t { VK_FORMAT_UNDEFINED = 0 };
typedef uint32_t VkFormatFeatureFlags;
constexpr uint32_t VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT = 0x80, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x200;
struct VkFormatProperties { uint32_t linearTilingFeatures, optimalTilingFeatures, bufferFeatures; };
struct { void* physical = (void*)1; } dev;
static void vkGetPhysicalDeviceFormatProperties(void*, VkFormat f, VkFormatProperties* p) {
 p->optimalTilingFeatures = f == 37 ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : f == 126 ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : 0;
}
static bool is_depth_format(VkFormat f) { return f >= 124 && f <= 130; }
'''+targets+'\n'+actual+r'''
static void write(const std::filesystem::path& path,const std::vector<StateRec>& records,size_t tail=0) {
 std::ofstream out(path,std::ios::binary);
 const uint32_t size=sizeof(StateRec);
 out.write(kStateMagic,8);out.write(reinterpret_cast<const char*>(&size),4);
 for(const auto& r:records)out.write(reinterpret_cast<const char*>(&r),sizeof r);
 if(tail){StateRec r{};out.write(reinterpret_cast<const char*>(&r),tail);}
 if(!out)throw std::runtime_error("synthetic file write failed");
}
int main(int argc,char** argv) {
 if(argc!=2)return 2;
 static_assert(sizeof(StateRec)==528);
 StateRec maximum{},zero{},bad_answers{},bad_questions{},huge{};
 maximum.code[0]=11;maximum.nanswers=24;maximum.nquestions=32;maximum.samples=1;maximum.n=8;maximum.nvattr=16;
 for(uint32_t i=0;i<24;i++)maximum.answers[i]=100+i;
 for(uint32_t i=0;i<32;i++)maximum.questions[i]=uint8_t(i);
 zero.code[0]=22;zero.samples=1;
 // (hookshot) the targets a pipeline is made with: at most 8, a sample count and topology Vulkan has, 16 attributes
 StateRec bad_n=maximum,neg_n=maximum,bad_samples=maximum,no_samples=maximum,bad_topo=maximum,bad_vattr=maximum;
 bad_n.n=9;neg_n.n=0xFFFFFFFFu;bad_samples.samples=3;no_samples.samples=0;bad_topo.topo=99;bad_vattr.nvattr=17;
 // (hookshot) formats the device draws to: any number reached the driver as a VkFormat
 StateRec drawn=maximum,bad_color=maximum,bad_depth=maximum;
 drawn.color_format[0]=37;drawn.has_depth=1;drawn.depth_format=126;
 bad_color.color_format[7]=0x7fffffffu;bad_depth.has_depth=1;bad_depth.depth_format=37;
 bad_answers=maximum;bad_answers.nanswers=25;
 bad_questions=maximum;bad_questions.nquestions=33;
 huge=maximum;huge.nanswers=huge.nquestions=std::numeric_limits<uint32_t>::max();
 int failures=0;
 auto check=[&](const std::filesystem::path& path,const std::vector<StateRec>& expected,const char* label){
  const auto got=state_list_read(path.string());
  bool equal=got.size()==expected.size();
  if(equal)for(size_t i=0;i<got.size();++i)equal&=std::memcmp(&got[i],&expected[i],sizeof(StateRec))==0;
  if(!equal){std::cerr<<label<<": reader returned "<<got.size()<<" records, expected "<<expected.size()<<'\n';++failures;}
 };
 // Both preparation inputs and state_record's read-before-append share this exact reader.
 for(const char* name:{"hookshot_pipeline_states.bin","hookshot_pipeline_states_shipped.bin"}){
  const auto path=std::filesystem::path(argv[1])/name;
  write(path,{maximum});check(path,{maximum},"valid maximum counts");
  write(path,{bad_answers});check(path,{},"answer count above array");
  write(path,{bad_questions});check(path,{},"question count above array");
  write(path,{huge});check(path,{},"maximum integer counts");
  write(path,{bad_n,neg_n,bad_samples,no_samples,bad_topo,bad_vattr});check(path,{},"targets a pipeline cannot have");
  write(path,{bad_n,maximum,bad_topo});check(path,{maximum},"retain a valid record between bad targets");
  write(path,{bad_color,drawn,bad_depth});check(path,{drawn},"formats the device does not draw to");
  write(path,{maximum,bad_answers,zero,bad_questions,maximum,huge,zero});
  check(path,{maximum,zero,maximum,zero},"retain valid records around rejected records");
  write(path,{maximum},sizeof(StateRec)-1);check(path,{maximum},"ignore truncated trailing record");
  write(path,{},sizeof(StateRec)-1);check(path,{},"truncated record only");
 }
 if(!failures)std::cout<<"PASS: actual reader bounds counts, retains valid records, ignores truncated tails; player and shipped names\n";
 return failures?1:0;
}
'''
    with tempfile.TemporaryDirectory(prefix='state-reader-counts-') as tmp:
        folder=Path(tmp);source=folder/'fixture.cpp';exe=folder/'fixture.exe'
        source.write_text(fixture,encoding='utf-8')
        subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe),str(folder)],check=True)

if __name__=='__main__':main()
