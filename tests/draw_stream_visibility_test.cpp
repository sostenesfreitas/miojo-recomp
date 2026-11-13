#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
static void require(bool ok,const char* why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
struct Allocation { std::vector<uint8_t> cpu,visible; };
using VmaAllocation=Allocation*;
struct { VkDevice device{}; VkQueue queue{}; VkPhysicalDeviceProperties props{}; std::mutex present_mu; } dev;
std::map<VkBuffer,Allocation> buffers;
uintptr_t serial=100;
template<class T> T handle() { return reinterpret_cast<T>(++serial); }
bool coherent=false,fail_buffer=false;
unsigned flushed=0,queued=0;
std::mutex par_mutex;
struct ParLock { std::lock_guard<std::mutex> lock{par_mutex}; };
void logf(const char*,...) {}
bool create_buffer(uint64_t size,VkBufferUsageFlags,bool,VkBuffer& buffer,VmaAllocation& allocation,void** mapped,bool=false) {
    if(fail_buffer) return false;
    buffer=handle<VkBuffer>(); auto& a=buffers[buffer]; a.cpu.assign((size_t)size,0x9a); a.visible.assign((size_t)size,0xc3);
    allocation=&a; *mapped=a.cpu.data(); return true;
}
void destroy_buffer(VkBuffer buffer,VmaAllocation allocation) { require(&buffers.at(buffer)==allocation,"allocation owner"); buffers.erase(buffer); }
void flush_mapped(VmaAllocation allocation,uint64_t size) {
    if(!size)return;
    require(allocation && size<=allocation->cpu.size(),"valid stream flush range");
    std::copy_n(allocation->cpu.begin(),(size_t)size,allocation->visible.begin()); flushed++;
}
/* RES */
std::deque<RecordingRes> res;
thread_local RecordingRes* cur=nullptr;
const uint64_t kStream=1024;
constexpr VkBufferUsageFlags kStreamUse=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
std::atomic<uint64_t> stream_extra{0},stream_grown{0},stream_peak{0};
bool stream_grow_off() { return false; }
struct BufferRef { VkBuffer buffer{}; uint64_t offset=0,size=0,address=0; };
void draw_flush_recording(int slot);
/* STREAM */
VkCommandBuffer rec_cmd{};
uint64_t rec_value=0,targets_serial=0;
VkBuffer zero_buffer{}; VmaAllocation zero_alloc{};
VkDescriptorPool make_pool() { return handle<VkDescriptorPool>(); }
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice,VkDescriptorPool,VkDescriptorPoolResetFlags) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice,VkDescriptorPool,const VkAllocationCallbacks*) {}
struct Pass {}; Pass& pass() { static Pass p;return p; }
bool work_since_barrier=false,last_dyn_valid=false,last_desc_valid=false,last_vb_valid=false;
VkPipeline last_pipeline{};
struct { bool on=false; } vertex_set,last_ib;
void descriptors_disturbed() {}
std::unordered_map<int,int>& handle_hits() { static std::unordered_map<int,int> h;return h; }
struct BankCache { uint64_t at=0; };
std::array<std::array<BankCache,2>,2>& bank_cache() { static std::array<std::array<BankCache,2>,2> b;return b; }
void make_dummies() {}
/* FLUSH */
/* BEGIN */
struct Recording { int slot;VkCommandBuffer cmd,upload,init; VkFence fence;uint64_t value=1,batch=0,part=0;bool submitted=false; };
struct Expected { BufferRef ref;std::vector<uint8_t> bytes; };
std::map<VkCommandBuffer,std::vector<Expected>> expected;
std::vector<int> submitted_slots;
std::map<VkCommandBuffer,int> slots;
bool texture_uploads_recorded() { return true; }
void gprof_submitted(int,bool) {}
void gpu_time_end(Recording&) {}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VkSemaphore timeline{},rec_timeline{};
uint64_t rec_seq=0;
std::mutex rec_map_mu;
// (the readback's part bookkeeping and the held-submit flag: gpu/vk/vk_replay.cpp)
uint64_t rec_part_next = 0;
std::map<uint64_t,uint64_t> part_rec;
std::condition_variable rec_map_cv;
std::atomic<bool> pend_held{false};
std::multimap<uint64_t,uint64_t> value_rec;
std::set<uint64_t> rec_open_values;
struct { std::vector<VkCommandBufferSubmitInfo> cbs;uint64_t signal=0,rec=0;bool any_signal=false;uint64_t reflex=0; } pend;
uint64_t reflex_frame() { return 0; }   // (vk_reflex.cpp: no Reflex here)
const void* reflex_submit_id(const void* next, uint64_t = ~0ull) { return next; }   // (vk_reflex.cpp: no tag without Reflex)
uint64_t batch_next=0;std::chrono::steady_clock::time_point pend_since;
bool batch=false;bool batch_on() { return batch; }
int flush_reason=0;constexpr int FR_CAP=1,FR_OTHER=0;bool batch_gpu_idle() { return false; }
void device_lost() { require(false,"unexpected device loss"); }
void note_timeline_sent(uint64_t) {}
#define PROF(x) ((void)0)
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue,uint32_t count,const VkSubmitInfo2* submits,VkFence) {
    for(unsigned i=0;i<count;i++) for(unsigned j=0;j<submits[i].commandBufferInfoCount;j++) {
        auto cmd=submits[i].pCommandBufferInfos[j].commandBuffer;
        if(slots.count(cmd))submitted_slots.push_back(slots.at(cmd));
        for(auto& e:expected[cmd]) {
            auto& a=buffers.at(e.ref.buffer);auto& seen=coherent?a.cpu:a.visible;
            require(e.ref.offset+e.bytes.size()<=seen.size(),"consumer bounds");
            require(std::equal(e.bytes.begin(),e.bytes.end(),seen.begin()+e.ref.offset),"GPU draw input must see the CPU-authored bytes");
        }
    }
    queued++;return VK_SUCCESS;
}
void queue_flush_locked() {
    if(pend.cbs.empty())return;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.commandBufferInfoCount=(uint32_t)pend.cbs.size();si.pCommandBufferInfos=pend.cbs.data();
    vkQueueSubmit2(dev.queue,1,&si,VK_NULL_HANDLE);pend.cbs.clear();
}
/* SUBMIT */
struct Job { Recording* r;int inits=0;bool uploads=true; };
void submit_segments(std::vector<Job>& jobs) {
    bool init_lost=false;
    for(auto& job:jobs) { Job* j=&job;
        /* SEGMENT */
    }
}
static Recording recording(int slot) {
    Recording r{slot,handle<VkCommandBuffer>(),handle<VkCommandBuffer>(),handle<VkCommandBuffer>(),handle<VkFence>()};
    slots[r.cmd]=slot; return r;
}
static void write(Recording& r,size_t bytes,unsigned seed) {
    std::vector<uint8_t> data(bytes);for(size_t i=0;i<bytes;i++)data[i]=uint8_t(i*37+seed*61);
    BufferRef ref=stream_copy(data.data(),data.size());require(ref.buffer!=VK_NULL_HANDLE && ref.size==bytes,"stream copy accepted");
    expected[r.cmd].push_back({ref,std::move(data)});
}
int main(int argc,char**argv) {
    require(argc==3,"fixture arguments");std::string mode=argv[1];coherent=std::string(argv[2])=="coherent";
    dev.props.limits.minUniformBufferOffsetAlignment=256;dev.props.limits.minStorageBufferOffsetAlignment=64;
    Recording a=recording(0);draw_begin_recording(0,a.cmd,1);
    if(mode=="empty") { submit_recording(a);require(!flushed,"empty recording must not flush old bytes");return 0; }
    write(a,701,1);
    if(mode=="grow" || mode=="reuse") {
        write(a,3107,2);write(a,1100011,3);require(res[0].si==2,"exercise multiple grown streams");
    }
    if(mode=="allocation-failure") {
        fail_buffer=true;std::vector<uint8_t> large(3000,9);auto missing=stream_copy(large.data(),large.size());fail_buffer=false;
        require(!missing.buffer && res[0].si==0,"failed growth must leave the current stream usable");
    }
    if(mode=="segments" || mode=="failed-segment") {
        Recording b=recording(1),c=recording(2);
        std::thread worker([&]{draw_begin_recording(1,b.cmd,1);write(b,997,4);write(b,3009,5);});worker.join();
        draw_begin_recording(2,c.cmd,1);write(c,487,6);require(cur==&res[2],"main thread current slot differs from submitted worker slots");
        std::vector<Job> jobs{{&a,1,true},{&b,mode=="failed-segment"?-1:0,true},{&c,0,false}};submit_segments(jobs);
        require(submitted_slots==(mode=="failed-segment"?std::vector<int>{0}:std::vector<int>{0,1,2}),"segment failure/ordering contract");return 0;
    }
    batch=mode=="batch";submit_recording(a);
    if(batch) {require(queued==0,"batch retained before queue use");queue_flush_locked();}
    if(mode=="reuse") {
        expected[a.cmd].clear();draw_begin_recording(0,a.cmd,2);require(res[0].si==0 && res[0].used==0 && res[0].streams.size()==3,"real begin resets used range and retains grown streams");
        write(a,69,7);submit_recording(a);
    }
    require(a.submitted,"healthy recording submitted");std::puts("PASS: production draw stream input visibility and submission paths");
}
