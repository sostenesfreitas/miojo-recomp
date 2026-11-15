"""Exercise the Builder's state-list import and cache save with game-free call stubs."""
import argparse, os, subprocess, tempfile
from pathlib import Path
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def block(source, marker):
    start = source.index(marker)
    at = source.index('{', start) + 1
    end, depth = at, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at:end - 1]


p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--cxx',default='clang++');a=p.parse_args()
body=block((a.source/'gpu/vk/vk_main.cpp').read_text(encoding='utf-8'), 'if (prepare_only()) {')
already_prepared=block((a.source/'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8'),
                       'if (why == PrepareWhy::None && !env_on("RX_VK_PREPARE_AGAIN")) {')
cpp=r"""
#include "vk_progress.h"
using namespace mkwvk;
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using std::size_t;
std::vector<int> calls;
bool empty_states=false;
void state_list_prepare(void(*progress)(size_t,size_t),bool open_seed=false){
 if(!open_seed||!progress)std::exit(7);
 calls.push_back(6);
 if(!empty_states){
  progress(1,4);
  progress(4,4);
 }
}
int seed_open(){std::exit(8);}
void logf(const char*,...){std::exit(9);}
bool warm_libraries(void(*)(size_t,size_t)){std::exit(10);}
struct PreparedFor {int warmed=1;};
void prepared_write(const std::string&, PreparedFor){std::exit(11);}
bool fresh_seed=true;
bool reuse_seed(void(*progress)(size_t,size_t),bool background,bool refresh_states){
 const PreparedFor already{};
 PreparedFor prepared_for=already;
 const int warmed=already.warmed;
 const bool libraries=false;
 const std::string warming="warming",path="prepared";
 """+already_prepared+r"""
}
bool prepare_run(void(*progress)(size_t,size_t),bool background=false,bool refresh_states=false){
 if(background||!refresh_states)std::exit(3);
 calls.push_back(1);
 return fresh_seed || reuse_seed(progress,background,refresh_states);
}
void predict_run(void(*)(size_t,size_t)){calls.push_back(5);}
bool predict_pending(){return false;}   // (no prediction left unfinished by a last start)
bool all_mode=true;
bool compile_all(){return all_mode;}   // (Compile shaders: All predicts after a preparation that translated; Most never)
void precompile_run(void(*progress)(size_t,size_t)){calls.push_back(2);progress(1,2);}
void pipeline_cache_save(bool force){if(!force)std::exit(2);calls.push_back(3);}
std::string skipped_path(){return "sentinel";}
void prepare_watchdog(){}   // (vk_main.cpp: the run's watchdog, tests/test_prepare_stall.py)
void prepare_moved(int,size_t,size_t){}
bool prepare_left_workers(){return false;}
int driver_faults(){return 0;}
[[noreturn]] void prepare_end(int){std::exit(12);}
void prepare_guess(size_t out[3]){out[0]=out[1]=out[2]=1;}
int fake_remove(const char*){calls.push_back(4);return 0;}
#define remove fake_remove
bool run(){
"""+body+r"""
}
int main(int argc,char** argv){
 if(argc!=3)return 1;
 const std::string mode=argv[1];
 fresh_seed=mode=="fresh";
 empty_states=mode=="empty";
 all_mode=std::string(argv[2])=="all";
 // (All: a fresh seed predicts (5); Most: never. A prepared seed imports the states (6) and does not predict either way)
 const std::vector<int> want=fresh_seed?(all_mode?std::vector<int>{1,5,2,3,4}:std::vector<int>{1,2,3,4}):std::vector<int>{1,6,2,3,4};
 if(!run()||calls!=want)return 2;
 calls.clear();
 if(reuse_seed(nullptr,false,false)||!calls.empty())return 3;
}
"""
with tempfile.TemporaryDirectory(prefix='prepare-cache-') as tmp:
    src=Path(tmp)/'fixture.cpp';exe=Path(tmp)/'fixture.exe';src.write_text(cpp)
    subprocess.run([a.cxx,'-std=c++20','-include',str(a.source/'gpu'/'vk'/'compat'/'hookshot_env.h'),'-I',str(a.source/'gpu'/'vk'),str(src),'-o',str(exe)],check=True)   # (env_on: the switches' rule, gpu/vk/compat/hookshot_env.h)
    env={k:v for k,v in os.environ.items() if k not in ('RX_VK_SEED_CHECK','RX_VK_PREPARE_AGAIN')}
    # (Most: the prediction's part of the bar goes after a fresh seed too, out of the total (0.600 at half the noted pipelines).
    # Left in, the step to the noted pipelines counted it done at its guess: the bar jumped by the prediction's share, 0.714)
    for mode, level, expected in [('fresh', 'all', ['0.714']), ('fresh', 'most', ['0.600']), ('refresh', 'all', ['0.125','0.500','0.750']),
                                  ('refresh', 'most', ['0.125','0.500','0.750']), ('empty', 'all', ['0.500']), ('empty', 'most', ['0.500'])]:
        result=subprocess.run([str(exe),mode,level],check=True,capture_output=True,text=True,env=env)
        progress=[line.split()[1] for line in result.stdout.splitlines() if line.startswith('@@progress ')]
        assert progress == expected, (mode, level, progress)
    print("PASS: Builder imports states with a prepared seed, reports progress, then compiles and saves the cache (All and Most)")
