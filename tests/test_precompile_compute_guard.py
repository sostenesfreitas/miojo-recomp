# SPDX-License-Identifier: GPL-3.0-or-later
"""Game-free Windows SEH check of the actual precompile compute arm.

Extracts production code; fake Vulkan never loads a GPU driver. The thread and
join below are fixture scaffolding, not the production scheduler/handover.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "clang++"]
RUN_ALL_SKIP = {"linux": "Windows structured exceptions"}


HEAD = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using VkResult=int; using VkPipeline=uintptr_t; using VkPipelineCache=uintptr_t;
constexpr VkResult VK_SUCCESS=0; constexpr uintptr_t VK_NULL_HANDLE=0;
constexpr int VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO=10;
constexpr int VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO=11;
constexpr int VK_SHADER_STAGE_COMPUTE_BIT=12;
constexpr int VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT=0x100;
struct VkComputePipelineCreateInfo {
 int sType; struct {int sType=0,stage=0;uintptr_t module=0;const char*pName=nullptr;} stage;
 uintptr_t layout=0; int flags=0;
};
// (no pipelineCreationCacheControl: the arm makes each pipeline with one call, the one guarded here. With it the arm asks the
// caches first, through the same cached_compute: tests/test_pipeline_state_computes.py)
struct {uintptr_t device=0x1234;struct{bool pipelineCreationCacheControl=false;}vk13;}dev;
VkPipelineCache pcache=0x5678;
struct Program {struct Stage{uintptr_t module;};std::vector<Stage>stages{{0x2468}};unsigned workgroup[3]{32,1,1};uintptr_t layout=0x1357;VkPipeline compute_pipeline=0xbeef;};
struct Pre {std::mutex mu;bool abandoned=false;std::atomic<size_t> computes_cached{0},computes_compiled{0},computes_failed{0};};
constexpr int VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT=0x1;
bool env_on(const char*){return false;}   // (RX_VK_NOOPT: not set, and the precompile does not follow it)
int mode=0,calls=0,destroys=0,logged=0; bool completed=false;
void logf(const char*,...){++logged;}
int faults_noted=0; void note_driver_fault(){++faults_noted;}
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c);std::exit(7);}}while(0)
const VkComputePipelineCreateInfo* expected_ci=nullptr; VkPipeline* expected_out=nullptr;
void pin_subgroup(decltype(VkComputePipelineCreateInfo::stage)&stage,unsigned x){CHECK(x==32);CHECK(stage.stage==12);}
VkResult vkCreateComputePipelines(uintptr_t device,VkPipelineCache cache,unsigned count,const VkComputePipelineCreateInfo*ci,const void*allocator,VkPipeline*out){
 CHECK(device==dev.device&&cache==pcache&&count==1&&allocator==nullptr&&out);
 CHECK(ci&&ci->sType==10&&ci->stage.sType==11&&ci->stage.stage==12&&ci->stage.module==0x2468&&ci->layout==0x1357);
 CHECK(ci->stage.pName&&std::string(ci->stage.pName)=="main");
 if(expected_ci)CHECK(ci==expected_ci); if(expected_out)CHECK(out==expected_out);
 ++calls;
 if(mode==1){*out=0xdead;RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);}   // (a handle written, then the fault)
 if(mode==2){*out=VK_NULL_HANDLE;return -4;}
 *out=0xcafe;return VK_SUCCESS;
}
void vkDestroyPipeline(uintptr_t device,VkPipeline made,const void*allocator){CHECK(device==dev.device&&made==0xcafe&&allocator==nullptr);++destroys;}
'''


def windows_target(cxx):
    """The target the game's own build gives this compiler (third_party/a64-kit/tools/toolchain.py native_target): an
    llvm-mingw clang++ builds for MinGW with -fms-extensions (__try/__except), any other for the MSVC target. llvm-mingw
    finds no Visual Studio headers, so the MSVC target alone fails there at <windows.h>."""
    machine=subprocess.run([cxx,'-dumpmachine'],capture_output=True,text=True,timeout=30).stdout.strip()
    if 'mingw' in machine or 'windows-gnu' in machine:
        return ['--target=x86_64-w64-mingw32','-fms-extensions']
    return ['--target=x86_64-pc-windows-msvc']


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--cxx',default='clang++')
    parser.add_argument('--linker',type=Path)
    parser.add_argument('--expect-fault',action='store_true',help='unfixed source must exit with injected access violation')
    args=parser.parse_args()
    if os.name != "nt":
        raise SystemExit("Windows structured exceptions required")
    source=(args.source/'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    wrapper=block(source,'__declspec(noinline) VkResult guarded_compute(')
    precompile=block(source,'void precompile_run(')
    arm=block(precompile,'if (i < computes.size()) {')
    # ci/made identities are checked at the boundary as well as inside the API spy.
    # (the arm takes its create info and its cache answer from these two, so the fixture takes them from the source as well)
    info=block(source,'VkComputePipelineCreateInfo compute_pipeline_info(')
    asked=block(source,'VkPipeline compute_pipeline_cached(')
    ask='VkPipeline made = compute_pipeline_cached(p, false);'
    armed=arm.replace(ask, ask+' expected_ci=&ci; expected_out=&made;')
    assert armed != arm
    # (the precompile runs before play, where cached_compute is guarded_compute with the start's cache: vk_draw.cpp create_cached)
    stand_in='\nVkResult cached_compute(const VkComputePipelineCreateInfo*ci,VkPipeline*pl){return guarded_compute(pcache,ci,pl);}\n'
    cpp=HEAD+wrapper+stand_in+info+'\n'+asked+'\nvoid actual_arm(Program&program,Pre*pre){std::vector<Program*>computes{&program};size_t i=0;'+armed+'\ncompleted=true;}\n'+r'''
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
 CHECK(argc==2);int which=std::atoi(argv[1]);Program program;Pre pre;
 if(which==4){
  mode=1;VkComputePipelineCreateInfo ci{10};ci.stage={11,12,0x2468,"main"};ci.layout=0x1357;VkPipeline out=0;
  expected_ci=&ci;expected_out=&out;
  CHECK(guarded_compute(pcache,&ci,&out)==-77);CHECK(calls==1&&out==0&&logged==1&&faults_noted==1);return 0;
 }
 mode=which==1?1:which==2?2:0;pre.abandoned=which==3;
 std::thread worker([&]{actual_arm(program,&pre);});worker.join();
 CHECK(completed&&calls==1);
 CHECK(pre.computes_cached==0&&pre.computes_compiled==(which==0||which==3?1u:0u)&&pre.computes_failed==(which==1||which==2?1u:0u));
 if(which==0)CHECK(program.compute_pipeline==0xcafe&&destroys==0);
 if(which==1||which==2)CHECK(program.compute_pipeline==0&&destroys==0);
 if(which==3)CHECK(program.compute_pipeline==0xbeef&&destroys==1);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='precompile-seh-') as tmp:
        path=Path(tmp); unit=path/'fixture.cpp';exe=path/'fixture.exe';unit.write_text(cpp,encoding='utf-8')
        cmd=[args.cxx,*windows_target(args.cxx),'-std=c++20',str(unit),'-o',str(exe),'-fuse-ld=lld']
        if args.linker:cmd+=['-B'+str(args.linker.parent)]
        subprocess.run(cmd,check=True,timeout=60)
        for case in [0,1,2,3,4]:
            result=subprocess.run([str(exe),str(case)],timeout=10)
            code=result.returncode & 0xffffffff
            expected=0xc0000005 if args.expect_fault and case==1 else 0
            if code!=expected:raise SystemExit(f'case {case}: exit {code:#x}, expected {expected:#x}')
            print(f'case {case}: exit {code:#x} PASS',flush=True)
    print('PASS: extracted wrapper/compute arm; modeled thread join only')


if __name__=='__main__':
    main()


