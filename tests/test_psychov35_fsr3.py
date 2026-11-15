"""Execute FSR 3.1's actual engine registration/HDR callback and the shared PsychoV35 format gate."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start)+1
    depth = 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    fsr = (args.source_root/'gpu/vk/vk_fsr3.cpp').read_text(encoding='utf-8')
    chain = (args.source_root/'gpu/vk/vk_temporal.cpp').read_text(encoding='utf-8')
    start = fsr.index('const TemporalEngine kChainEngine{')
    registration = fsr[start:fsr.index('};', start)+2]
    callback = function(fsr, 'bool chain_hdr(') if 'bool chain_hdr(' in fsr else ''
    fixture = r'''
#include <cassert>
#include <cstdint>
#include "gpu/vk/upscaler_rows.h"
namespace ur=mkwvk::upscaler_rows;
namespace upscaler_rows=mkwvk::upscaler_rows;
bool display_hdr=false,enabled=false;int hdr=0,tone=0,mode=ur::FSR3;
bool scene_tonemap_on(){return display_hdr && hdr==2 && tone==1;}
int temporal_mode(){return mode;}
bool temporal_off(){return mode==ur::OFF;}
int env_switch(const char*){return enabled?1:0;}
using VkFormat=int;
constexpr int VK_FORMAT_B10G11R11_UFLOAT_PACK32=122,VK_FORMAT_R16G16B16A16_SFLOAT=97;
struct TemporalEngine {
 const char* name;const char* short_name;bool(*start)();int(*prepare)();bool(*evaluate)();
 void(*release)();uint64_t(*memory)();bool(*hdr)()=nullptr;bool fsr_depth=false,retry=false;
};
bool chain_start(){return true;}int chain_prepare(){return 1;}bool chain_evaluate(){return true;}
void chain_release(){}uint64_t chain_memory(){return 0;}
struct { bool hdr = false; } chain_state_v;   /* (Hookshot's chain state: no native HDR presented here) */
auto& chain_state() { return chain_state_v; }
/* CALLBACK */
/* REGISTRATION */
/* GATE */
/* PAIR */
int main(int argc,char**){
 enabled=argc>1;
 assert(kChainEngine.hdr); // Main has no HDR callback, so never takes PsychoV35.
 assert(kChainEngine.fsr_depth && kChainEngine.retry);
 for(int d=0;d<2;++d)for(hdr=0;hdr<3;++hdr)for(tone=0;tone<2;++tone){
  display_hdr=d;const bool psycho=enabled && display_hdr && hdr==2 && tone==1;
  assert(kChainEngine.hdr()==psycho); // Vanilla and SDR retain their original preparation.
  assert(temporal_scene_format(97)==psycho);
  assert(temporal_colour_pair(97,97)==psycho);
  assert(temporal_colour_pair(122,122));
  assert(!temporal_colour_pair(97,122) && !temporal_colour_pair(122,97));
 }
 display_hdr=true;hdr=2;tone=1;mode=ur::GAME;assert(!temporal_scene_format(97)); // Legacy own-input route.
 // Off has a separate bypass policy; it must not become an HDR temporal engine.
 // Do not require its independent format gate to reject a buffer after the Off fix merges.
 mode=ur::OFF;assert(!ur::takes_psychov35(mode));
 mode=ur::FSR3;assert(temporal_scene_format(97)==enabled);
}
'''.replace('/* CALLBACK */', callback).replace('/* REGISTRATION */', registration)
    fixture = fixture.replace('/* GATE */', function(chain, 'bool temporal_scene_format('))
    fixture = fixture.replace('/* PAIR */', function(chain, 'bool temporal_colour_pair('))
    with tempfile.TemporaryDirectory(prefix='psycho-fsr3-') as folder:
        cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(args.source_root), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        subprocess.run([str(exe), 'enabled'], check=True)
    print('FSR 3.1 PsychoV35: real HDR registration/callback, unchanged Vanilla and matching format policy pass')


if __name__ == '__main__':
    main()
