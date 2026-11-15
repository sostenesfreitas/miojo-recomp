"""Execute the independent camera admission and both real AMD colour-constant blocks, without a GPU."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]


def block(text, signature):
    a = text.index(signature)
    b = text.index('{', a)+1
    depth = 1
    while depth:
        depth += (text[b] == '{')-(text[b] == '}')
        b += 1
    return text[a:b]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cxx', default='g++')
    p.add_argument('--source', type=Path, default=ROOT)
    a = p.parse_args()
    inputs = (a.source/'gpu/vk/vk_fsr3_inputs.cpp').read_text(encoding='utf-8')
    fi = (a.source/'gpu/vk/vk_fsr3fi.cpp').read_text(encoding='utf-8')
    camera = block(inputs, 'struct Camera {')+';'
    helpers = '\n'.join(block(inputs, sig) for sig in ['bool wanted()', 'void note_scene_draw('])
    for signature in ['bool scene_format(', 'bool colour_pair(']:
        if signature in inputs:
            helpers = block(inputs, signature)+'\n'+helpers
    # The new pair helper refers to scene_format; provide its declaration before either definition.
    helpers = ('bool scene_format(int);\n' if 'bool scene_format(' in inputs else '')+helpers
    color = ''
    pair_cases = ''
    if 'bool colour_pair(' in inputs:
        pair_cases = '''
 fg=1;tone=true;assert(colour_pair(97,97)==enabled);assert(colour_pair(122,122));
 assert(!colour_pair(97,122) && !colour_pair(122,97) && !colour_pair(44,44));
 tone=false;assert(!colour_pair(97,97));tone=true;fg=0;assert(!colour_pair(97,97));
 fg=1;for(int multiplier:{0,1,3,8}){opts.framegen=multiplier;assert(!colour_pair(97,97));}
'''
    if 'void colour_constants(' in fi:
        color = '\n'.join(block(fi, s)+';' for s in ['struct Constants {', 'struct OfConstants {'])
        # The SDK's std140 cbFI/cbOF fields; size alone would not catch a misplaced HDR pair.
        color += '\nstatic_assert(offsetof(Constants,back_buffer_transfer_function)==140 && offsetof(Constants,min_max_luminance)==144);'
        color += '\nstatic_assert(offsetof(OfConstants,back_buffer_transfer_function)==20 && offsetof(OfConstants,min_max_luminance)==24);'
        color += '\n'+block(fi, 'void colour_constants(')
        start = fi.index('    const float max_luminance = in.native_pq')
        end = fi.index('    const int32_t rw =', start)
        color += '\nstruct {bool first_frame=true,native_pq=false;float max_luminance=1;uint64_t sequence=0;} g;\n'
        color += 'struct Input {bool reset=false,native_pq=false;float max_luminance=1;uint64_t sequence=1;};\n'
        color += 'bool reset_for(const Input& in){'+fi[start:end]+'return reset;}\n'
        begin = fi.index('    if (in.native_pq &&')
        finish = fi.index('    if (!prepare(', begin)
        color += 'bool accepted(const Input& in){'+fi[begin:finish]+'return true;}\n'
        color += r'''
void color_cases(){
 Constants k{};OfConstants of{};
 for(float peak:{400.f,1000.f,4000.f,10000.f}){
  colour_constants(k,of,true,peak);
  assert(k.back_buffer_transfer_function==1 && of.back_buffer_transfer_function==1);
  assert(k.min_max_luminance[0]==0 && of.min_max_luminance[0]==0);
  assert(k.min_max_luminance[1]==peak && of.min_max_luminance[1]==peak);
 }
 colour_constants(k,of,false,1000);
 assert(k.back_buffer_transfer_function==0 && of.back_buffer_transfer_function==0);
 assert(k.min_max_luminance[1]==1 && of.min_max_luminance[1]==1);
 Input in;assert(reset_for(in));++in.sequence;assert(!reset_for(in));
 in.native_pq=true;
 for(float invalid:{0.f,-1.f,10001.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
  in.max_luminance=invalid;assert(!accepted(in));
 }
 in.max_luminance=10000;assert(accepted(in));in.native_pq=false;
 ++in.sequence;in.native_pq=true;in.max_luminance=1000;assert(reset_for(in));
 ++in.sequence;assert(!reset_for(in));++in.sequence;in.max_luminance=4000;assert(reset_for(in));
 ++in.sequence;assert(!reset_for(in));++in.sequence;in.native_pq=false;assert(reset_for(in));
 ++in.sequence;in.max_luminance=7000;assert(!reset_for(in)); // SDR ignores HDR peak.
 in.sequence+=2;assert(reset_for(in));++in.sequence;in.reset=true;assert(reset_for(in));
}
'''
    else:
        color = 'void color_cases(){assert(false && "missing AMD HDR colour constants");}'
    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <initializer_list>
#include <cmath>
#include <limits>
#include <cstddef>
#include "gpu/vk/upscaler_rows.h"
namespace upscaler_rows = mkwvk::upscaler_rows;
using VkFormat=int;
constexpr int VK_FORMAT_B10G11R11_UFLOAT_PACK32=122,VK_FORMAT_R16G16B16A16_SFLOAT=97;
struct Texture{int format=122;};
bool tone=false,enabled=false;unsigned fg=0;uint64_t frame=10;
struct {int framegen=2;} opts;
bool scene_tonemap_on(){return tone;}
int env_switch(const char*){return enabled?1:0;}
bool fsr3_wanted(){return false;}
namespace fsr3fi {unsigned frames_wanted(){return fg;}}
uint64_t texture_frame_no(){return frame;}
uint64_t texture_destroy_generation(){return 1;}
bool memory_read_as_submitted(uint64_t,float* c,size_t n){
 memset(c,0,n);c[0x120/4]=1280;c[0x124/4]=720;c[0xa8/4]=-1;return true;
}
template<class...T>void logf(T...){}
/* CAMERA */
struct State{std::mutex mu;Camera cam;} state;
State& st(){return state;}
/* HELPERS */
/* COLOR */
int main(int argc,char**){
 enabled=argc>1;Texture depth;
 for(int t=0;t<2;++t)for(unsigned wanted:{0u,1u})for(int format:{122,VK_FORMAT_R16G16B16A16_SFLOAT,44}){
  tone=t;fg=wanted;state.cam={};Texture source;source.format=format;
  note_scene_draw(&source,1280,720,&depth,1,0x230);
  assert(state.cam.valid==(wanted && (format==122 || (format==97 && tone && enabled))));
 }
 /* PAIR_CASES */
 color_cases();
}
'''.replace('/* CAMERA */', camera).replace('/* HELPERS */', helpers).replace('/* COLOR */', color).replace('/* PAIR_CASES */', pair_cases)
    with tempfile.TemporaryDirectory(prefix='psycho-fsr3-fg-') as folder:
        cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([a.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe), 'enabled'], check=True)
        subprocess.run([str(exe)], check=True)
    if 'void colour_constants(' in fi:
        generate = block(fi, 'bool generate(')
        assert 'colour_constants(k, of, in.native_pq, max_luminance);' in generate
        assert generate.index('colour_constants(k, of,') < generate.index('// This evaluation owns its constants')
    print('FSR FG: actual camera admission, both SDK colour blocks and encoding/peak/sequence resets pass')


if __name__ == '__main__':
    main()
