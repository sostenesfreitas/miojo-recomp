"""Execute both admission gates and FSR's HDR callback with real environment values in separate processes."""
import argparse
import os
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
    p = argparse.ArgumentParser()
    p.add_argument('--cxx', default='g++')
    p.add_argument('--source', type=Path, default=ROOT)
    a = p.parse_args()
    temporal = (a.source/'gpu/vk/vk_temporal.cpp').read_text(encoding='utf-8')
    independent = (a.source/'gpu/vk/vk_fsr3_inputs.cpp').read_text(encoding='utf-8')
    fsr = (a.source/'gpu/vk/vk_fsr3.cpp').read_text(encoding='utf-8')
    menu = (a.source/'gpu/vk/vk_menu.cpp').read_text(encoding='utf-8')
    code = r'''
#include <cassert>
#include <cstdlib>
#include <initializer_list>
#include "gpu/vk/upscaler_rows.h"
namespace ur=mkwvk::upscaler_rows;
namespace upscaler_rows=mkwvk::upscaler_rows;
using VkFormat=int;
constexpr int VK_FORMAT_B10G11R11_UFLOAT_PACK32=122,VK_FORMAT_R16G16B16A16_SFLOAT=97;
struct {bool hdr=false;} dev;
struct {int hdr=0,hdr_tonemap=0,framegen=0;} opts;
int mode=ur::GAME,fg=0;
int temporal_mode(){return mode;}
bool temporal_off(){return mode==ur::OFF;}
int env_switch(const char* name){const char* s=std::getenv(name);return s && *s?std::atoi(s):-1;}
namespace fsr3fi {int frames_wanted(){return fg;}}
/* TONE */
struct { bool hdr = false; } chain_state_v;   /* (Hookshot's chain state: no native HDR presented here) */
auto& chain_state() { return chain_state_v; }
/* HDR_CALLBACK */
/* TEMPORAL */
namespace independent {
/* INDEPENDENT */
}
int main(int argc,char**){
 const bool enabled=argc>1;
 for(int display:{0,1})for(int hdr:{0,1,2})for(int tone:{0,1}){
  dev.hdr=display;opts.hdr=hdr;opts.hdr_tonemap=tone;
  const bool psycho=display && hdr==2 && tone==1;
  assert(chain_hdr()==(enabled && psycho));
  for(mode=0;mode<ur::kUpscalers;++mode)for(int in:{0,44,97,122})for(int out:{0,44,97,122}){
   const bool rgba=psycho && (mode==ur::OFF || (enabled && ur::takes_psychov35(mode)));
   assert(temporal_scene_format(in)==(in==122 || (rgba && in==97)));
   assert(temporal_colour_pair(in,out)==((in==122 && out==122)||(rgba && in==97 && out==97)));
   for(int multiplier:{0,1,2,3,8})for(int wanted:{0,1}){
    opts.framegen=multiplier;fg=wanted;
    const bool fi_rgba=enabled && psycho && multiplier==2 && wanted;
    assert(independent::scene_format(in)==(in==122 || (fi_rgba && in==97)));
    assert(independent::colour_pair(in,out)==(in==out && (in==122 || (fi_rgba && in==97))));
   }
  }
 }
}
'''
    code = code.replace('/* TONE */', function(menu, 'bool scene_tonemap_on('))
    code = code.replace('/* HDR_CALLBACK */', function(fsr, 'bool chain_hdr('))
    code = code.replace('/* TEMPORAL */', '\n'.join(function(temporal, s) for s in
                        ['bool temporal_scene_format(', 'bool temporal_colour_pair(']))
    code = code.replace('/* INDEPENDENT */', '\n'.join(function(independent, s) for s in
                        ['bool scene_format(', 'bool colour_pair(']))
    with tempfile.TemporaryDirectory(prefix='psycho-default-') as folder:
        cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test.exe'
        cpp.write_text(code, encoding='utf-8')
        subprocess.run([a.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(a.source), str(cpp), '-o', str(exe)], check=True)
        for value in (None, '', '0', '1', '2'):
            env = os.environ.copy()
            env.pop('RX_VK_PSYCHOV35_DLSS', None)
            if value is not None:
                env['RX_VK_PSYCHOV35_DLSS'] = value
            subprocess.run([str(exe), *(['enabled'] if value!='0' else [])], env=env, check=True)
    print('PsychoV35 default: both gates, FSR HDR callback, Native HDR selection, unset/empty/0/1/2, formats and 2x limits pass')


if __name__ == '__main__':
    main()
