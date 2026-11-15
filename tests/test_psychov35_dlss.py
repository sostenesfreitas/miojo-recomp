"""Compile the actual opt-in scene gate and exercise HDR/mode/format transitions without a game."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cxx', default='g++')
    p.add_argument('--write', action='store_true', help='regenerate the RGBA16F shader header with glslang')
    p.add_argument('--glslang', default='glslang')
    a = p.parse_args()
    if a.write:
        shader = ROOT/'gpu/vk/shaders/dlss_out.comp'
        header = ROOT/'gpu/vk/shaders/dlss_out_rgba_comp_spv.h'
        subprocess.run([a.glslang, '-V', '--target-env', 'vulkan1.3', '-DDLSS_RGBA16F=1',
                        '--vn', 'DLSS_OUT_RGBA_COMP_SPV', '-o', str(header), str(shader)], check=True)
        digest = hashlib.sha256(shader.read_bytes()).hexdigest()
        header.write_text('// Generated from dlss_out.comp; SHA-256: ' + digest + '\n' +
                          header.read_text(encoding='utf-8'), encoding='utf-8', newline='\n')
    chain = (ROOT / 'gpu/vk/vk_temporal.cpp').read_text(encoding='utf-8')
    draw = (ROOT / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    gate = function(chain, 'bool temporal_scene_format(')
    pair = function(chain, 'bool temporal_colour_pair(')
    switch = re.search(r'    if \(s\.psycho_last != psycho \|\| s\.history_input != in->format\) \{.*?\n    \}', chain, re.S).group()
    fixture = r'''
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <string>
#include "gpu/vk/upscaler_rows.h"
namespace ur = mkwvk::upscaler_rows;
using VkFormat = int;
constexpr int VK_FORMAT_B10G11R11_UFLOAT_PACK32=122, VK_FORMAT_R16G16B16A16_SFLOAT=97;
int mode=ur::DLSS;
bool tone=false;
int temporal_mode() { return mode; }
bool scene_tonemap_on() { return tone; }
int env_switch(const char* n) { const char* e=std::getenv(n); return e && *e ? std::atoi(e) : -1; }
unsigned logs=0;
std::string route;
std::string engine_name;
void logf(const char*, const char* engine, const char* value, unsigned, unsigned, unsigned) { ++logs; engine_name=engine; route=value; }
/* GATE */
/* PAIR */
struct S { bool psycho_last=false; unsigned history_input=122; unsigned long long last_frame=30; };
void transition(S& s, bool psycho, unsigned format=122) {
  struct { unsigned scene_format=97; } cam;
  struct T { unsigned format=122; } ti, to;
  ti.format=format;
  to.format=format;
  T *in=&ti, *out=&to;
  const bool rgba=psycho && format==97;
/* SWITCH */
}
int main(int argc, char**) {
  const bool enabled=argc>1;
  for (mode=0; mode<ur::kUpscalers; ++mode) {
    for (int t=0;t<2;++t) {
      tone=t;
      for (int f=0;f<256;++f) {
        const bool expected=f==122 || (enabled && tone && ur::takes_psychov35(mode) && f==97);
        assert(temporal_scene_format(f)==expected);
        for (int output=0;output<256;++output) {
          const bool correct=(f==122 && output==122) ||
              (enabled && tone && ur::takes_psychov35(mode) && f==97 && output==97);
          assert(temporal_colour_pair(f,output)==correct);
        }
      }
    }
  }
  // DLSS/DLAA and FSR 4 / its native AA take it; FSR 3.1 (own packed inputs), the game's network and Off never do
  assert(ur::takes_psychov35(ur::DLSS) && ur::takes_psychov35(ur::DLAA) && ur::takes_psychov35(ur::FSR4) &&
         ur::takes_psychov35(ur::FSR4_AA));
  assert(!ur::takes_psychov35(ur::GAME) && !ur::takes_psychov35(ur::FSR3) && !ur::takes_psychov35(ur::OFF));
  S s;
  s.history_input=0; // First Vanilla evaluation resets history without a Psycho/HDR log.
  transition(s,false); assert(s.last_frame==0 && logs==0);
  s.last_frame=30;
  transition(s,false); assert(s.last_frame==30);
  transition(s,true); assert(s.last_frame==0 && s.psycho_last && logs==1 && route=="packed");
  s.last_frame=60; transition(s,true); assert(s.last_frame==60);
  transition(s,false); assert(s.last_frame==0 && !s.psycho_last);
  s.last_frame=80; transition(s,true,97); assert(s.last_frame==0 && s.history_input==97);
  assert(route=="RGBA16F linear");
  s.last_frame=90; transition(s,true,97); assert(s.last_frame==90);
  transition(s,true,122); assert(s.last_frame==0 && s.history_input==122);
  assert(route=="packed" && engine_name=="DLSS");
  mode=ur::FSR4_AA; s.last_frame=95; transition(s,true,97);
  assert(s.last_frame==0 && route=="RGBA16F linear" && engine_name=="FSR 4");
}
'''.replace('/* GATE */', gate).replace('/* PAIR */', pair).replace('/* SWITCH */', switch)
    # All three scene consumers agree: camera, animated characters, reactive mask.
    assert '!temporal_scene_format(color0->format)' in function(chain, 'void temporal_note_scene_draw(')
    assert '!temporal_scene_format(at.color_format[0])' in function(draw, 'static void skin_check(')
    assert '!temporal_scene_format(at.color_format[0])' in function(draw, 'void reactive_scene_draw(')
    upscale = function(chain, 'bool temporal_upscale(')
    assert '!temporal_scene_format(cam.scene_format)' in upscale
    assert '!temporal_scene_format(in->format)' in upscale
    assert '!temporal_colour_pair(in->format, out->format)' in upscale
    assert 'psycho && (!engine.e->hdr || !engine.e->hdr())' in upscale
    assert 'ur::takes_psychov35(mode)' in upscale and 'ur::takes_psychov35(temporal_mode())' in gate
    assert upscale.index('PsychoV35 waits') < upscale.index('engine.e->evaluate(cmd, f)')
    assert 'prep_hdr_data{16384.0f, VK_TRUE}' in chain
    assert 'out_hdr_square = VK_FALSE' in chain
    assert 'const VkPipeline prep_pipe = hdr ? s.prep_hdr : s.prep;' in upscale
    assert 'const VkPipelineLayout prep_layout = hdr ? s.prep_hdr_layout : s.prep_layout;' in upscale
    assert 'rgba ? s.out_rgba' in upscale
    assert 'if (rgba && !make_rgba_output())' in upscale
    # Both translated and rejected networks must use a storage view matching the typed output shader.
    stand_in = function(draw, 'bool temporal_stand_in(')
    assert 'ViewUse::Sample, VK_FORMAT_R16G16B16A16_SFLOAT' in stand_in
    assert 'ViewUse::Storage, VK_FORMAT_R16G16B16A16_SFLOAT' in stand_in
    rejected = function(draw[draw.rindex('static bool temporal_rejected_network('):], 'static bool temporal_rejected_network(')
    assert 'rgba_pair ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B10G11R11_UFLOAT_PACK32' in rejected
    reactive = (ROOT/'gpu/vk/vk_reactive.cpp').read_text(encoding='utf-8')
    assert '!temporal_scene_format(color->format)' in function(reactive, 'bool copy_scene(')
    # FG consumes the successful chain's same frame, including the history reset. It must not be skipped for RGBA16F.
    handoff = 'dlss_fg_inputs(cmd, f, cam.projection, cam.current, cam.jitter, motion);'
    assert upscale.index('f.reset = reset;') < upscale.index('engine.e->evaluate(cmd, f)') < upscale.index(handoff)
    dlss = (ROOT/'gpu/vk/vk_dlss.cpp').read_text(encoding='utf-8')
    inputs = function(dlss, 'void dlss_fg_inputs(')
    assert 'g.have_last && motion && !f.reset' in inputs
    assert 'vkCmdCopyImage(cmd, f.depth.image' in inputs and 'vkCmdCopyImage(cmd, f.motion.image' in inputs
    assert 'B10G11R11' not in inputs and 'f.color' not in inputs
    rgba_pipe = function(chain, 'bool make_rgba_output(')
    assert 'square = VK_FALSE' in rgba_pipe and 'DLSS_OUT_RGBA_COMP_SPV' in rgba_pipe
    # Changing storage precision must not skip the network's perceptual-to-linear decode.
    prep=(ROOT/'gpu/vk/shaders/dlss_prep.comp').read_text()
    assert 'kSquare ? e * e : e' in prep
    # Validate the compiled storage image type, not just a source-file string.
    header=(ROOT/'gpu/vk/shaders/dlss_out_rgba_comp_spv.h').read_text()
    shader=(ROOT/'gpu/vk/shaders/dlss_out.comp').read_bytes()
    assert hashlib.sha256(shader).hexdigest() in header.splitlines()[0]
    words=[int(w,16) for w in re.findall(r'0x[0-9a-fA-F]{8}',header)]
    assert words[0]==0x07230203
    i=5
    formats=[]
    while i<len(words):
        count,opcode=words[i]>>16,words[i]&65535
        assert count>0 and i+count<=len(words)
        if opcode==25: formats.append(words[i+8])
        i+=count
    assert 2 in formats and 8 not in formats, formats  # Rgba16f, never packed R11fG11fB10f.
    with tempfile.TemporaryDirectory(prefix='psychov35-dlss-') as folder:
        cpp, exe = Path(folder)/'test.cpp', Path(folder)/'test.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([a.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT), str(cpp), '-o', str(exe)], check=True)
        for value in ('', '0', '1', '2'):
            env = os.environ.copy()
            env['RX_VK_PSYCHOV35_DLSS'] = value
            subprocess.run([str(exe), *(['enabled'] if value=='1' else [])], env=env, check=True)
    print('PsychoV35 DLSS and FSR 4: 3,670,016 input/output combinations, scene gate, history and RGBA SPIR-V checks passed')

if __name__ == '__main__':
    main()
