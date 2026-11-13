"""Check prepared answer domains using actual Program/Stage/Eden descriptor types; no game/GPU.

python -I -B tests/test_program_answer_domains.py --headers <Vulkan-Headers/include> [--cxx clang++]
Uses the source-extraction/temp-compiler pattern of test_3d_view_create_flags.py and
real headers as test_lower_depbar.py does. Requires configured Clang/GCC and Vulkan headers;
no Vulkan library, loader, shader binary, game data or prebuilt object is used.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "clang++", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


FIXTURE = r'''
// SPDX-License-Identifier: GPL-3.0-or-later
// Real Hookshot Program/Stage and Eden descriptors; only resource-boundary values are stubbed.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "vk_shader.h"
namespace mkwvk {
static uint32_t handles[3]{};
static uint8_t answers[4]{0,2,2,2};
uint32_t driver_bank_word(const NvnState&, int, int, uint32_t offset) { return handles[offset/8]; }
uint32_t stage_cbuf_word(const NvnState&, const Stage&, uint32_t, uint32_t offset) { return handles[offset/8]; }
uint32_t stage_handle(const NvnState&, const Stage&, uint32_t) { std::abort(); }
uint8_t texture_answer(uint32_t h) { return h ? answers[h] : 0xff; }
uint32_t texture_slot_version(int id) { return 10+id; }
#include MATCHER_FUNCTIONS
static unsigned failed=0,checked=0;
static void check(bool b,const char* what){checked++;if(!b){failed++;fprintf(stderr,"FAIL: %s\n",what);}}
static void unbind(){handles[0]=handles[1]=handles[2]=0;answers[1]=answers[2]=answers[3]=2;}
static void bind(){handles[0]=1;handles[1]=2;handles[2]=3;}
static std::unique_ptr<Program> image(Shader::ImageFormat fmt,bool integer=true){
 auto p=std::make_unique<Program>();p->stages.emplace_back();auto& info=p->stages[0].info;
 for(unsigned i=0;i<2;i++){Shader::TextureDescriptor d{};d.type=Shader::TextureType::Color2D;d.count=1;d.cbuf_index=2;d.cbuf_offset=8*i;info.texture_descriptors.push_back(d);}
 Shader::ImageDescriptor d{};d.type=Shader::TextureType::Color2D;d.count=1;d.cbuf_index=2;d.cbuf_offset=16;d.format=fmt;d.is_integer=integer;info.image_descriptors.push_back(d);return p;
}
static std::unique_ptr<Program> sampled(bool depth,bool integer=false){
 auto p=std::make_unique<Program>();p->stages.emplace_back();Shader::TextureDescriptor d{};d.type=Shader::TextureType::ColorArray2D;d.count=1;d.cbuf_index=2;d.is_depth=depth;d.is_integer=integer;p->stages[0].info.texture_descriptors.push_back(d);return p;
}
}
int main(int argc, char** argv){using namespace mkwvk;auto s=std::make_unique<NvnState>();
 if(argc>1){
  unbind();auto p=image(Shader::ImageFormat::R32_UINT);record_answers(*p,*s);auto hash=p->variant;
  check(p->answers[2].type==128,"strict UINT prepared metadata unchanged");bind();
  check(!still_valid(*s,*p),"strict UINT compare remains raw");record_answers(*p,*s);
  check(p->answers[2].type==0&&p->variant!=hash,"strict UINT record/hash remains raw");
  answers[3]=130;check(!still_valid(*s,*p),"strict UINT changed raw bit still rejects");
  unbind();auto d=sampled(true);record_answers(*d,*s);hash=d->variant;
  check(d->answers[0].type==3,"strict depth prepared metadata unchanged");bind();answers[1]=131;
  check(!still_valid(*s,*d),"strict depth compare remains raw");record_answers(*d,*s);
  check(d->answers[0].type==131&&d->variant!=hash,"strict depth record/hash remains raw");
  answers[1]=3;check(!still_valid(*s,*d),"strict depth changed raw bit still rejects");
  unbind();auto f=sampled(false);f->stages[0].info.texture_descriptors[0].type_fixed=true;record_answers(*f,*s);bind();
  answers[1]=2;check(!still_valid(*s,*f),"strict instruction-typed compare remains raw");
  printf("strict checks=%u failures=%u\n",checked,failed);return failed?1:0;
 }

 for(auto fmt:{Shader::ImageFormat::R8_UINT,Shader::ImageFormat::R16_UINT,Shader::ImageFormat::R32_UINT,Shader::ImageFormat::R32G32_UINT,Shader::ImageFormat::R32G32B32A32_UINT}){
  unbind();auto p=image(fmt);record_answers(*p,*s);auto hash=p->variant;check(p->answers.size()==3&&p->answers[2].type==128,"prepared UINT answer metadata retained");
  bind();std::vector<uint32_t> before,after;answer_handles(*s,*p,before);auto mask=answer_stage_mask(*p);
  check(still_valid(*s,*p),"correct UINT module accepts float-base integer bit");record_answers(*p,*s);
  check(p->answers[2].type==128&&p->variant==hash,"UINT record/hash canonical across bound bit");
  answers[3]=130;check(still_valid(*s,*p),"correct UINT module accepts integer-base bit");record_answers(*p,*s);check(p->variant==hash,"UINT integer bound hash stays canonical");
  answer_handles(*s,*p,after);check(before==after&&before==std::vector<uint32_t>({1,11,2,12,3,13}),"all UINT handle/version entries preserved");
  check(answer_stage_mask(*p)==mask&&mask==1,"UINT binding-stage mask preserved");
  answers[1]=130;check(!still_valid(*s,*p),"ordinary sampled integer dependency beside UINT retained");
 }
 unbind();auto depth=sampled(true);record_answers(*depth,*s);auto hash=depth->variant;check(depth->answers[0].type==3,"depth canonical dimension3 retained");bind();answers[1]=131;
 check(still_valid(*s,*depth),"correct float depth accepts integer-bit-only change");record_answers(*depth,*s);check(depth->answers[0].type==3&&depth->variant==hash,"depth record/hash canonical");
 answers[1]=130;check(!still_valid(*s,*depth),"depth dimension dependency preserved");
 unbind();auto wrong_depth=sampled(true,true);record_answers(*wrong_depth,*s);bind();answers[1]=3;check(!still_valid(*s,*wrong_depth),"wrong integer-depth module not broadened to float");
 unbind();auto wrong_uint=image(Shader::ImageFormat::R32_UINT,false);record_answers(*wrong_uint,*s);bind();answers[3]=130;check(!still_valid(*s,*wrong_uint),"wrong float-UINT module not broadened to integer");
 for(auto fmt:{Shader::ImageFormat::Typeless,Shader::ImageFormat::R8_SINT,Shader::ImageFormat::R16_SINT}){
  for(bool integer:{false,true}){unbind();auto p=image(fmt,integer);record_answers(*p,*s);auto old=p->variant;bind();answers[3]=integer?2:130;check(!still_valid(*s,*p),"Typeless/signed actual integer-bit changes preserved");record_answers(*p,*s);check(p->variant!=old,"Typeless/signed hashes still distinguish integer bit");}
 }
 unbind();auto ordinary=sampled(false);record_answers(*ordinary,*s);bind();answers[1]=131;check(!still_valid(*s,*ordinary),"ordinary sampled integer change retained");answers[1]=2;check(!still_valid(*s,*ordinary),"dimension change retained where an answer set the type");handles[0]=0;check(still_valid(*s,*ordinary),"unbound handling preserved");
 // The instruction's own type (TextureDescriptor::type_fixed): the module and the view are the same whatever is bound.
 unbind();auto fixed=sampled(false);fixed->stages[0].info.texture_descriptors[0].type_fixed=true;record_answers(*fixed,*s);auto fixed_hash=fixed->variant;bind();
 answers[1]=2;check(still_valid(*s,*fixed),"instruction-typed slot accepts another dimension");record_answers(*fixed,*s);
 check(fixed->answers[0].type==3&&fixed->variant==fixed_hash,"instruction-typed record/hash canonical across dimension");
 answers[1]=131;check(!still_valid(*s,*fixed),"instruction-typed slot keeps the integer bit");
 answers[1]=130;check(!still_valid(*s,*fixed),"instruction-typed slot keeps the integer bit across dimension");
 answers[1]=8;check(!still_valid(*s,*fixed),"a rectangle is still another module");
 answers[1]=7;check(!still_valid(*s,*fixed),"a buffer is still another module");
 unbind();auto fixed_depth=sampled(true);fixed_depth->stages[0].info.texture_descriptors[0].type_fixed=true;record_answers(*fixed_depth,*s);bind();
 answers[1]=130;check(still_valid(*s,*fixed_depth),"instruction-typed float depth accepts dimension and integer-bit change");
 unbind();auto counted=image(Shader::ImageFormat::R32_UINT);for(auto& d:counted->stages[0].info.texture_descriptors)d.count=2;counted->stages[0].info.image_descriptors[0].count=2;record_answers(*counted,*s);check(counted->answers.empty()&&counted->variant==0,"count!=1 descriptors still omitted");
 // Hash-only callers (seed/census cache readers) have no stage metadata; do not change their contract.
 Program hash_only;hash_only.answers={{0,0,2,false},{0,1,2,false},{0,0,128,true}};check(answers_hash(hash_only)!=0,"answers_hash remains usable without module Info");
 printf("checks=%u failures=%u\n",checked,failed);return failed?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT, help='repository source tree')
    parser.add_argument('--shader-source', type=Path, help='override only vk_shader.cpp for before/after checks')
    parser.add_argument('--cxx-arg', action='append', default=[], help='extra compiler flag (repeatable)')
    args = parser.parse_args()
    vk = args.source.resolve() / 'gpu/vk'
    shader = args.shader_source or vk / 'vk_shader.cpp'
    source = shader.read_text(encoding='utf-8-sig')
    markers = [
        'uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::TextureDescriptor& d)',
        'uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::ImageDescriptor& d)',
        'uint32_t answer_handle(', 'uint8_t answer_of(',
    ]
    # The baseline has no canonical helper: execute its real functions and fail behavioral checks.
    if 'uint8_t canonical_answer(' in source:
        markers.append('uint8_t canonical_answer(')
    if 'bool answers_fit(' in source:   # (the answers from files checked against the stages and their slots)
        markers.append('bool answers_fit(')
    if 'uint8_t declared_answer(' in source:   # (what an unbound slot answers: record_answers' and program_find_plain's)
        markers.append('uint8_t declared_answer(')
    markers += ['uint64_t answers_hash(', 'size_t generated_stage(', 'std::vector<Program::Answer> answers_here(', 'void record_answers(', 'bool still_valid(',
                'void answer_handles(', 'uint32_t answer_stage_mask(']
    functions = '\n\n'.join(block(source, marker) for marker in markers)
    if 'd.type_fixed' in functions:   # (the baseline has no such rule: nothing to hold the pass to)
        texture_pass = (vk / 'eden/shader_recompiler/ir_opt/texture_pass.cpp').read_text(encoding='utf-8')
        for needed in ('.type_fixed = !type_asked,', '].type_fixed &= desc.type_fixed;'):
            if needed not in texture_pass:
                raise SystemExit('texture_pass.cpp no longer sets or merges TextureDescriptor::type_fixed: ' + needed)
    if 'if (answers_in' in functions:
        start = source.rfind('\n', 0, source.index(' answers_in =')) + 1
        functions = source[start:source.index(';', start) + 1] + '\n\n' + functions
    fixture = FIXTURE.replace('#include MATCHER_FUNCTIONS', functions)
    flags = ['-std=c++20', '-include', ENV_ON_H, '-O1', '-w', '-DNDEBUG', '-DVK_NO_PROTOTYPES', '-DMKWVK_INCLUDES=2']
    for header in ('algorithm', 'cstdio', 'cstdlib', 'cstring', 'functional', 'string', 'vector'):
        flags += ['-include', header]
    includes = [vk, vk / 'compat', args.source / 'gpu/compat', vk / 'eden',
                args.source / 'gpu/third_party/suyu', args.headers]
    if os.name != 'nt':
        includes.insert(0, args.source / 'host/posix')
    flags += ['-I' + str(path.resolve()) for path in includes]
    with tempfile.TemporaryDirectory(prefix='program-answers-') as temp:
        folder = Path(temp)
        cpp, exe = folder / 'fixture.cpp', folder / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, *flags, *args.cxx_arg, str(cpp), '-o', str(exe)],
                       cwd=folder, check=True)
        failed = False
        # Each child reads the existing settings once, just like canonical_answer's static gate.
        for name, overrides, strict in (
            ('default', {}, False),
            ('handles-live-zero', {'RX_VK_HANDLES_LIVE': '0'}, False),   # (an on/off switch too: env_on)
            ('handles-live-one', {'RX_VK_HANDLES_LIVE': '1'}, True),
            ('register-at-once-zero', {'RX_VK_REGISTER_AT_ONCE': '0'}, False),   # (an on/off switch: 0 is off, env_on)
            ('register-at-once-one', {'RX_VK_REGISTER_AT_ONCE': '1'}, True),
            ('capture-zero', {'RX_VK_CAPTURE': '0'}, True),
            ('capture-one', {'RX_VK_CAPTURE': '1'}, False),
        ):
            env = os.environ.copy()
            for key in ('RX_VK_HANDLES_LIVE', 'RX_VK_REGISTER_AT_ONCE', 'RX_VK_CAPTURE'):
                env.pop(key, None)
            env.update(overrides)
            print(name, flush=True)
            result = subprocess.run([str(exe)] + (['strict'] if strict else []), cwd=folder, env=env)
            failed |= result.returncode != 0
        if failed:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
