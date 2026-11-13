"""Compile actual Reflex consumers with deterministic swapchain-retirement interleavings. No GPU/SDK."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
p.add_argument('--cxx', default='clang++')
a = p.parse_args()
source = (a.source/'gpu/vk/vk_reflex.cpp').read_text()

def function(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

functions = '\n'.join(function(marker) for marker in [
    'bool on()', 'void mark(', 'bool reflex_swapchain_stale()',
    'void reflex_swapchain_created(', 'void reflex_swapchain_destroyed()',
    'void reflex_present_begins()', 'void reflex_submit_ends()',
    'const void* reflex_present_id(', 'void reflex_presented()', 'void reflex_sleep()',
    'uint64_t reflex_frame()', 'void reflex_game_submitted()', 'const void* reflex_submit_id(',
])
fixture = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include "hookshot_env.h"
#include "gpu_wait_stats.h"
namespace gpu_wait_stats = mkwvk::gpu_wait_stats;
using VkSwapchainKHR=uint64_t;using VkLatencyMarkerNV=int;
constexpr uint64_t VK_NULL_HANDLE=0;
constexpr int VK_SUCCESS=0,VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV=1,
    VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO=2,VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV=3,
    VK_LATENCY_MARKER_SIMULATION_START_NV=4,VK_LATENCY_MARKER_INPUT_SAMPLE_NV=5,
    VK_LATENCY_MARKER_SIMULATION_END_NV=6,VK_LATENCY_MARKER_RENDERSUBMIT_START_NV=7,
    VK_LATENCY_MARKER_RENDERSUBMIT_END_NV=8,VK_LATENCY_MARKER_PRESENT_START_NV=9,VK_LATENCY_MARKER_PRESENT_END_NV=10,
    VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV=11;
struct VkLatencySubmissionPresentIdNV {int sType;const void* pNext=nullptr;uint64_t presentID=0;};
struct VkLatencySleepInfoNV {int type;uint64_t signalSemaphore=0,value=0;};
struct VkSemaphoreWaitInfo {int type;unsigned semaphoreCount=0;const uint64_t* pSemaphores=nullptr;const uint64_t* pValues=nullptr;};
struct VkSetLatencyMarkerInfoNV {int type;uint64_t presentID=0;int marker=0;};
struct PresentId {const void* pNext=nullptr;unsigned swapchainCount=0;const uint64_t* pPresentIds=nullptr;};
void (*before_lock)()=nullptr;
struct TestMutex {
    std::mutex native;
    void lock(){auto hook=before_lock;before_lock=nullptr;if(hook)hook();native.lock();}
    void unlock(){native.unlock();}
};
int markers=0,sleeps=0,modes=0;bool invalid=false,fg=false;int wanted=1;uint64_t marked_id=0;
void marker_call(int,uint64_t chain,const VkSetLatencyMarkerInfoNV* i){if(!chain)invalid=true;++markers;marked_id=i->presentID;}
int sleep_call(int,uint64_t chain,const VkLatencySleepInfoNV*){if(!chain)invalid=true;++sleeps;return VK_SUCCESS;}
struct Reflex {
    bool ready=true,latency=true,latency_next=true,frame_marked=true;
    uint64_t swapchain=42,swapchain_generation=0,wake=100,wake_value=0,id=1;
    int mode=1;TestMutex mu;PresentId pid;uint64_t pid_value=0,presenting=0,last_present_id=0;
    void (*marker)(int,uint64_t,const VkSetLatencyMarkerInfoNV*)=marker_call;
    int (*sleep)(int,uint64_t,const VkLatencySleepInfoNV*)=sleep_call;
} state;
struct {int device=1;}dev;
Reflex& rx(){return state;}
bool framegen_on(){return fg;}
int wanted_mode(){return wanted;}
void apply_mode(){if(!state.swapchain)invalid=true;++modes;}
void log_timings(){}
template<class... T> void logf(const char*,T...){}
int vkWaitSemaphores(int,const VkSemaphoreWaitInfo*,uint64_t);
constexpr uint64_t kReflexThisFrame=~0ull;thread_local uint64_t reflex_present_frame=kReflexThisFrame;
const void* reflex_submit_id(const void* next,uint64_t frame=kReflexThisFrame);   // (vk_reflex.h's default)
''' + functions + r'''
enum Action {Keep,Retire,Replace,ReuseHandle,LaterSleep,Disable,EnableFG};
Action action=Keep;
int vkWaitSemaphores(int,const VkSemaphoreWaitInfo* wait,uint64_t){
    const uint64_t requested=*wait->pValues;
    if(action==Retire || action==Replace || action==ReuseHandle){
        auto old=state.swapchain;reflex_swapchain_destroyed();
        if(action!=Retire)reflex_swapchain_created(action==ReuseHandle?old:99);
    }
    if(action==LaterSleep)++state.wake_value;
    if(action==Disable)wanted=0;
    if(action==EnableFG)fg=true;
    if(*wait->pValues!=requested)invalid=true; // The wait must use its own timeline target.
    return VK_SUCCESS;
}
void reset(){
    state.swapchain=42;state.swapchain_generation=0;state.ready=state.latency=state.latency_next=state.frame_marked=true;
    state.wake_value=0;state.id=1;state.mode=1;markers=sleeps=modes=0;invalid=fg=false;wanted=1;before_lock=nullptr;action=Keep;
    state.pid_value=state.presenting=state.last_present_id=0;marked_id=0;reflex_present_frame=kReflexThisFrame;
}
void retire_before_lock(){reflex_swapchain_destroyed();}
int main(){
    reset();reflex_sleep();if(invalid||markers!=2||sleeps!=1||!state.frame_marked)return 1;
    for(auto change:{Retire,Replace,ReuseHandle,LaterSleep,Disable}){
        reset();action=change;reflex_sleep();
        if(invalid||markers){std::puts("FAIL: a completed wait marked a retired/replaced/ineligible chain");return 2;}
        if((change==Retire||change==Replace||change==ReuseHandle)&&state.frame_marked)return 3;
    }
    // Retirement between the eligibility check and its lock must stop every consumer.
    reset();before_lock=retire_before_lock;reflex_sleep();if(invalid||sleeps||modes||markers)return 4;
    reset();before_lock=retire_before_lock;reflex_present_begins();if(invalid||markers)return 5;
    reset();before_lock=retire_before_lock;reflex_submit_ends();if(invalid||markers)return 6;
    reset();before_lock=retire_before_lock;int next=1;if(reflex_present_id(&next)!=&next||invalid||markers)return 7;
    reset();before_lock=retire_before_lock;reflex_presented();if(invalid||markers)return 8;
    reset();action=ReuseHandle;reflex_sleep();action=Keep;reflex_sleep();if(invalid||markers!=2||!state.frame_marked)return 9;
    reset();reflex_present_begins();reflex_submit_ends();reflex_present_id(nullptr);reflex_presented();
    if(invalid||markers!=5)return 10;
    // Frame generation: the game's frames are still marked and paced (a wait that turns it on marks the frame too)
    reset();action=EnableFG;reflex_sleep();if(invalid||markers!=2||!state.frame_marked)return 11;
    reset();fg=true;reflex_present_begins();if(markers!=2)return 12;
    reset();fg=true;reflex_game_submitted();if(markers!=1||reflex_frame()!=1)return 13;
    reset();state.frame_marked=false;if(reflex_frame()!=0)return 14;
    // its display thread: a made frame gets no present id and no markers; the set's game frame its own id, going up only
    reset();fg=true;state.id=7;reflex_present_frame=0;
    if(reflex_present_id(&next)!=&next||markers)return 15;
    reflex_presented();reflex_submit_ends();if(markers)return 16;   // (nor do the display thread's copies end the game's submission)
    reflex_present_frame=5;
    const auto* id5=static_cast<const PresentId*>(reflex_present_id(&next));
    if(id5!=&state.pid||id5->pNext!=&next||id5->swapchainCount!=1||*id5->pPresentIds!=5||markers!=1||marked_id!=5)return 17;
    reflex_presented();if(markers!=2||marked_id!=5)return 18;
    if(reflex_present_id(&next)!=&next||markers!=2)return 19;   // (5 again: not going up)
    reflex_present_frame=kReflexThisFrame;   // (then frame generation off: the frame being made, 7, goes up from 5)
    if(reflex_present_id(&next)!=&state.pid||*state.pid.pPresentIds!=7||marked_id!=7)return 20;
    // a queue submit's tag (reflex_submit_id): the frame its GPU work belongs to; none where there is no such frame.
    // RX_VK_REFLEX_SUBMIT_IDS follows hookshot_env.h's rule: this runs once with =on (tags on) and once with =0 (off)
    if(getenv("EXPECT_SUBMIT_IDS_OFF")){reset();state.id=3;if(reflex_submit_id(&next)!=&next)return 29;std::puts("PASS: RX_VK_REFLEX_SUBMIT_IDS=0 leaves the submits untagged");return 0;}
    using Tag=VkLatencySubmissionPresentIdNV;
    reset();state.id=3;
    const auto* t=static_cast<const Tag*>(reflex_submit_id(&next));   // (the frame being made)
    if(t==(const void*)&next||t->sType!=VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV||t->pNext!=&next||t->presentID!=3)return 21;
    t=static_cast<const Tag*>(reflex_submit_id(nullptr,2));   // (a frame kept since: a batch's first, a set's game frame)
    if(!t||t->pNext||t->presentID!=2)return 22;
    if(reflex_submit_id(&next,0)!=&next)return 23;   // (a made frame's copy: none)
    state.frame_marked=false;if(reflex_submit_id(&next)!=&next)return 24;   // (no frame marked yet)
    reset();state.id=3;state.last_present_id=3;
    if(reflex_submit_id(&next)!=&next)return 25;   // (frame 3 shown already: no new work for it)
    t=static_cast<const Tag*>(reflex_submit_id(&next,4));if(t==(const void*)&next||t->presentID!=4)return 26;
    reset();wanted=0;if(reflex_submit_id(&next)!=&next)return 27;   // (Reflex off)
    reset();before_lock=retire_before_lock;if(reflex_submit_id(&next)!=&next||invalid||markers)return 28;   // (retired before its lock)
    std::puts("PASS: Reflex retirement before locks and across waits, reused handles, stable wait targets and recovery; "
              "with frame generation the game's frames paced, made frames without present ids, the game frame's own id; "
              "queue submits tagged with their game frame, none for made frames, unmarked or shown frames, or Reflex off");
}
'''
with tempfile.TemporaryDirectory(prefix='reflex-swapchain-') as tmp:
    cpp, exe = Path(tmp)/'fixture.cpp', Path(tmp)/'fixture.exe'
    cpp.write_text(fixture)
    subprocess.run([a.cxx, '-std=c++20', '-O1', '-I' + str(a.source/'gpu/vk/compat'),
                    '-I' + str(Path(__file__).resolve().parents[1]/'gpu/vk'), str(cpp), '-o', str(exe)], check=True)
    # (=on keeps the tags: an atoi(...) == 0 reading took it as off; =0 turns them off)
    rc = subprocess.run([str(exe)], env=dict(os.environ, RX_VK_REFLEX_SUBMIT_IDS='on')).returncode
    if rc == 0:
        rc = subprocess.run([str(exe)], env=dict(os.environ, RX_VK_REFLEX_SUBMIT_IDS='0', EXPECT_SUBMIT_IDS_OFF='1')).returncode
    raise SystemExit(rc)
