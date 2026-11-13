"""Run actual pool lookup/range guards/prefetch queueing with fake GPU/background dependencies.

No Vulkan, background import thread, SDK or game data is used.

python nx2/test_import_windows.py [--cxx clang++]
"""
import argparse,os
from pathlib import Path
import subprocess,tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT=Path(__file__).resolve().parents[1]
def block(text,marker):
    start=text.index(marker);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cxx',default='clang++');a=p.parse_args()
    source=(ROOT/'gpu/vk/vk_memory.cpp').read_text(encoding='utf-8')
    guard=source[source.index('bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {'):source.index('    p.lo = lo;',source.index('bool import_pool('))]
    lookup=block(source,'Pool* pool_lookup(uint64_t guest, uint64_t size) {')
    chunking=source[source.index('uint64_t import_chunk() {'):source.index('Pool* pool_lookup(uint64_t guest, uint64_t size) {')]
    cache=block(source,'Pool* pool_of(uint64_t guest, uint64_t size) {')
    prefetch=block(source,'void prefetch_chunks(uint64_t lo, uint64_t hi, uint64_t chunk) {')
    fixture='''#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL:%d %s\\n",__LINE__,#x);std::exit(1);} }while(0)
struct Pool { uint64_t lo=0,hi=0,range_lo=0,range_hi=0,address=0; bool prefetched=false,gpu_only=false,gpu_written=false; uint8_t trims=0; uint64_t trim_epoch=0; };
struct GuestPool { uint64_t lo,hi; };
std::vector<GuestPool> guest_pools;
std::map<uint64_t,Pool> imported;
Pool *last_pool=nullptr,*pool_cache[4096]{};
uint64_t max_import_size=128ull<<20;
int failures=0,imports=0,retires=0,bg_starts=0;
int64_t st_bg_waits=0,st_prefetch_used=0,st_prefetch_sent=0,st_prefetch_dropped=0;
struct { bool external_memory_host=true; } dev;
std::mutex bg_mu;std::condition_variable bg_done_cv,bg_cv;std::vector<uint64_t> bg_inflight;
std::deque<std::pair<uint64_t,uint64_t>> bg_queue;
void bg_start() { bg_starts++; } // Recording spy: no thread or GPU import is started.
void logf(const char*,...){} void bg_drain(){}
void guest_pages_set(uint64_t,uint64_t,uint64_t){}
void retire_pool(std::map<uint64_t,Pool>::iterator it) { imported.erase(it);last_pool=nullptr;std::fill(std::begin(pool_cache),std::end(pool_cache),nullptr);retires++; }
enum { VK_SUCCESS = 0 };
thread_local bool import_cooling = false;
thread_local int import_result = 0;
bool import_on_hold(uint64_t, uint64_t) { import_cooling = false; return false; }
void import_remember_failure(uint64_t, uint64_t) {}
bool mirror_on() { return true; } // (import_chunk's default: 4 MB with VRAM mirrors, 16 without)
'''+guard+'''
    p.lo=lo;p.hi=hi;imports++;return true;
}
'''+prefetch+'\n'+chunking+'\n'+lookup+'\n'+cache+'''
int check_prefetch(int mode) {
    const uint64_t base=0x1000000000ull,chunk=16ull<<20;
    guest_pools.push_back({base,base+8*chunk});
    dev.external_memory_host=mode!=2;
    Pool seed;seed.lo=base+3*chunk;seed.hi=base+4*chunk;seed.prefetched=true;
    auto &ready=imported.emplace(seed.lo,seed).first->second;
    CHECK(pool_lookup(seed.lo+64,64)==&ready && !ready.prefetched && st_prefetch_used==1 && imports==0);
    if(mode==1) {
        CHECK(bg_queue.size()==2 && bg_starts==2 && st_prefetch_sent==2);
        CHECK(bg_queue[0].first==base+4*chunk && bg_queue[0].second==base+5*chunk);
        CHECK(bg_queue[1].first==base+2*chunk && bg_queue[1].second==base+3*chunk);
    } else CHECK(bg_queue.empty() && bg_starts==0 && st_prefetch_sent==0);
    CHECK(pool_lookup(seed.lo+128,64)==&ready && st_prefetch_used==1 && imports==0);
    Pool plain;plain.lo=base+6*chunk;plain.hi=base+7*chunk;
    auto &unprefetched=imported.emplace(plain.lo,plain).first->second;
    CHECK(pool_lookup(plain.lo+64,64)==&unprefetched && !unprefetched.prefetched && st_prefetch_used==1 && imports==0);
    CHECK(bg_starts==(mode==1?2:0) && st_prefetch_sent==(mode==1?2:0));
    auto demand=pool_lookup(base+64,64);
    CHECK(demand && !demand->prefetched && imports==1 && st_prefetch_used==1);
    CHECK(bg_queue.size()==(mode==1?3u:0u) && bg_starts==(mode==1?3:0) && st_prefetch_sent==(mode==1?3:0));
    CHECK(bg_inflight.size()==bg_queue.size() && st_prefetch_dropped==0 && st_bg_waits==0);
    std::printf("PASS: prefetched first use, reuse, unprefetched lookup, neighbour queueing (mode %d)\\n",mode);
    return 0;
}
int main(int argc,char **argv) {
    if(argc>1) return check_prefetch(std::atoi(argv[1]));
    const uint64_t base=0x1000000000ull,extent=0x99200000ull;
    guest_pools.push_back({base,base+extent});
    const auto chunk=uint64_t(std::atoi(std::getenv("RX_VK_IMPORT_CHUNK_MB")))<<20;
    if(!chunk) {
        CHECK(!pool_lookup(base+1234,64) && imports==0);
        max_import_size=4ull<<30;
        auto p=pool_lookup(base+1234,64);CHECK(p && p->lo==base && p->hi==base+extent);
    } else {
        const uint64_t address=base+1234,expected=address/chunk*chunk;
        auto p=pool_lookup(address,64);CHECK(p && p->lo==expected && p->hi==expected+chunk && imports==1);
        CHECK(pool_lookup(address+100,64)==p && imports==1);
        p=pool_lookup(expected+4*chunk+123,64);CHECK(p && p->lo==expected+4*chunk && p->hi==expected+5*chunk && imports==2);
        p=pool_lookup(expected+chunk-16,64);CHECK(p && p->lo==expected && p->hi==expected+2*chunk && retires==1);
        if(chunk==(16ull<<20)) {
            // a game pool across a chunk's end: its first binding imports both chunks (span_game_pools)
            const uint64_t far=base+0x40000000ull,far2=base+0x50000000ull;
            guest_pools.push_back({far+chunk-4096,far+chunk+4096});
            guest_pools.push_back({far2+chunk-4096,far2+chunk+4096});
            p=pool_lookup(far+chunk-64,32);CHECK(p && p->lo==far && p->hi==far+2*chunk);
            CHECK(pool_lookup(far+chunk+64,32)==p);   // (the binding across the end: no import again)
            // a spanned import that fails (past the device's limit here) is made again without the span
            const uint64_t limit=max_import_size;max_import_size=chunk;
            p=pool_lookup(far2+chunk-64,32);CHECK(p && p->lo==far2 && p->hi==far2+chunk);
            max_import_size=limit;
        }
    }
    const int before=imports;
    CHECK(!pool_lookup(base+extent+1,1));
    CHECK(!pool_lookup(base+extent-1,2));
    CHECK(!pool_lookup(base,UINT64_MAX));
    CHECK(!pool_lookup(base,0));
    CHECK(!pool_of(base,UINT64_MAX) && !pool_of(base,0) && imports==before);
    guest_pools.push_back({UINT64_MAX-1024,UINT64_MAX});
    if(chunk) CHECK(!pool_lookup(UINT64_MAX-16,8) && imports==before);
    Pool rejected;CHECK(!import_pool(base,base,rejected) && !import_pool(base+1,base,rejected));
    CHECK(!import_pool(base,base+max_import_size+1,rejected));
    std::printf("PASS: request windows, reuse, overlap retirement, range/overflow guards, import limits (chunk %llu)\\n",(unsigned long long)chunk);
}
'''
    work=ROOT/'work';work.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='import-windows-',dir=work) as temp:
        directory=Path(temp).resolve();directory.relative_to(work.resolve());cpp=directory/'fixture.cpp';exe=directory/'fixture.exe';cpp.write_text(fixture,encoding='utf-8')
        subprocess.run([a.cxx,'-std=c++20','-O2','-D_CRT_SECURE_NO_WARNINGS',str(cpp),'-o',str(exe)],check=True)
        for chunk in ('16','3','0'):
            subprocess.run([str(exe)],env=dict(os.environ,RX_VK_IMPORT_CHUNK_MB=chunk,RX_VK_PREFETCH_CHUNKS='0'),check=True)
        for mode in ('1','0','2'):
            subprocess.run([str(exe),mode],env=dict(os.environ,RX_VK_IMPORT_CHUNK_MB='16',RX_VK_PREFETCH_CHUNKS='0' if mode=='0' else '1'),check=True)
if __name__=='__main__':main()
