# SPDX-License-Identifier: GPL-3.0-or-later
"""Check production texture/image handle accessors with synthetic constant banks."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]


# These production bodies have balanced braces; literals in them contain no braces.
def function(text, signature):
    start = text.find(signature)
    assert start >= 0, f'Missing function anchor: {signature}'
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--cxx', default='c++')
    a = p.parse_args()
    shader = (a.source_root / 'gpu/vk/vk_shader.cpp').read_text(encoding='utf-8')
    draw = (a.source_root / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    extracted = '\n'.join([
        function(shader, 'uint32_t stage_cbuf_word('),
        function(draw, 'template <class D>\nuint32_t handle_of('),
        function(shader, 'uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::TextureDescriptor& d)'),
        function(shader, 'uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::ImageDescriptor& d)'),
    ])
    prelude = r'''
#define _CRT_SECURE_NO_WARNINGS 1
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
constexpr int kNvnStages=8;
struct nvn_p_bind_buf { uint64_t addr=0, size=0; };
struct NvnState { nvn_p_bind_buf ubo[kNvnStages][16]{}; };
struct Stage { int nvn_stage=2; std::vector<uint8_t> c1; };
namespace Shader {
struct TextureDescriptor {
 uint32_t handle_program=~0U,cbuf_index=1,cbuf_offset=0,size_shift=3;
 bool has_secondary=false;
 uint32_t shift_left=0,secondary_cbuf_index=3,secondary_cbuf_offset=0,secondary_shift_left=8;
};
struct ImageDescriptor { uint32_t handle_program=~0U,cbuf_index=1,cbuf_offset=0,size_shift=3; };
}
uint32_t driver_bank_word(const NvnState&,int bank,int stage,uint32_t offset) {
 return bank==0 || bank==2 ? 0x1000U+uint32_t(bank*256+stage*16)+offset : 0;
}
uint8_t recorded[32]{},live_bytes[32]{};
int recorded_reads=0,live_reads=0,vm_reads=0;
void ub_read(uint64_t addr,void* out,size_t n) { ++recorded_reads; std::memcpy(out,recorded+addr-4096,n); }
struct Host { void gpu_read(uint64_t addr,void* out,size_t n) { ++live_reads; std::memcpy(out,live_bytes+addr-4096,n); } } host_object;
Host* host=&host_object;
uint32_t stage_handle(const NvnState&,const Stage&,uint32_t program) { ++vm_reads; return 0xABC000U+program; }
'''
    checks = r'''
int main() {
 int failures=0;
 auto eq=[&](uint32_t actual,uint32_t expected,const char* label) {
  if(actual!=expected) { std::cerr<<label<<": "<<actual<<" != "<<expected<<'\n'; ++failures; }
 };
 NvnState s; Stage st; st.c1.resize(16);
 for(uint32_t off=0;off<16;off+=4) {
  uint32_t v=1+off; std::memcpy(st.c1.data()+off,&v,4);
  v=2+off; std::memcpy(recorded+off,&v,4);
  v=3+off; std::memcpy(live_bytes+off,&v,4);
 }
 s.ubo[2][0]={4096,16};
 const bool live=std::getenv("RX_VK_HANDLES_LIVE")!=nullptr;
 Shader::TextureDescriptor t; Shader::ImageDescriptor i;
 auto all=[&](uint32_t bank,uint32_t offset,uint32_t expected,const char* label) {
  t={}; i={}; t.cbuf_index=i.cbuf_index=bank; t.cbuf_offset=i.cbuf_offset=offset;
  eq(handle_of(s,st,t,0),expected,label);
  eq(handle_of(s,st,i,0),expected,label);
  eq(descriptor_handle(s,st,t),expected,label);
  eq(descriptor_handle(s,st,i),expected,label);
 };
 all(1,4,5,"inline c1"); all(3,4,live?7:6,"bound user bank");
 all(0,4,driver_bank_word(s,0,2,4),"driver c0");
 all(2,4,driver_bank_word(s,2,2,4),"driver c2");
 all(1,13,0,"short inline"); all(3,13,0,"bounded user");
 all(4,0,0,"unbound user"); all(19,0,0,"unsupported bank");
 s.ubo[2][0].size=0; all(3,12,live?15:14,"unsized binding"); s.ubo[2][0].size=16;
 t={}; t.has_secondary=true;
 eq(descriptor_handle(s,st,t),1U|((live?3U:2U)<<8),"secondary user");
 eq(handle_of(s,st,t,1),9U|((live?11U:10U)<<8),"array step primary and secondary");
 t.cbuf_index=0; t.secondary_cbuf_index=1;
 eq(descriptor_handle(s,st,t),driver_bank_word(s,0,2,0)|(1U<<8),"secondary inline");
 t.secondary_cbuf_index=2;
 eq(descriptor_handle(s,st,t),driver_bank_word(s,0,2,0)|(driver_bank_word(s,2,2,0)<<8),"driver pair");
 st.nvn_stage=-1; all(3,0,0,"invalid user stage"); st.nvn_stage=2;
 t={}; i={}; t.handle_program=i.handle_program=3;
 const int before=recorded_reads+live_reads;
 eq(handle_of(s,st,t,9),0xABC003,"VM array bypass");
 eq(descriptor_handle(s,st,t),0xABC003,"VM texture bypass");
 eq(descriptor_handle(s,st,i),0xABC003,"VM image bypass");
 eq(vm_reads,3,"VM call count"); eq(recorded_reads+live_reads,before,"VM no ordinary reads");
 eq(live?recorded_reads:live_reads,0,"unchanged memory source");
 if(!failures) std::cout<<"bank-reader checks passed (live="<<live<<")\n";
 return failures?1:0;
}
'''
    with tempfile.TemporaryDirectory(prefix='hookshot-bank-reader-') as tmp:
        src = Path(tmp) / 'check.cpp'
        exe = Path(tmp) / ('check.exe' if os.name == 'nt' else 'check')
        src.write_text(prelude + extracted + checks, encoding='utf-8')
        env_on_h = str(a.source_root / 'gpu' / 'vk' / 'compat' / 'hookshot_env.h')   # (env_on: RX_VK_HANDLES_LIVE)
        subprocess.run([a.cxx, '-std=c++20', '-include', env_on_h, '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)],
                       check=True, timeout=60)
        for live in (False, True):
            env = os.environ.copy()
            env.pop('RX_VK_HANDLES_LIVE', None)
            if live:
                env['RX_VK_HANDLES_LIVE'] = '1'
            result = subprocess.run([str(exe)], env=env, check=True, capture_output=True, text=True, timeout=10)
            assert result.stdout.strip() == f'bank-reader checks passed (live={int(live)})', 'Fixture did not report completed bank checks'
            print(result.stdout, end='')


if __name__ == '__main__':
    main()
