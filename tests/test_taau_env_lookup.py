"""Actual-source TAAU key/full stand-in spy; GPU boundaries are spies, no Vulkan loader or performance measurement.

Use --emit-only FILE to stage C++ without invoking a compiler.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "hookshot_env.h")


def block(source, marker):
    start = source.index(marker)
    return source[start:start + re.search(r'^}', source[start:], re.M).end()]


def fixture(source):
    return (PREFIX + block(source, 'bool taau_key(uint64_t key) {') + '\n' +
            block(source, 'bool taau_stand_in(VkCommandBuffer cmd,') + '\n' + SUFFIX)


PREFIX = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static const char *configured_keys, *no_f16;
static int key_reads;
static const char* fixture_getenv(const char* name) {
    if (!strcmp(name, "RX_VK_TAAU_KEYS")) { ++key_reads; return configured_keys; }
    if (!strcmp(name, "RX_VK_NO_F16")) return no_f16;
    return nullptr;
}
#define getenv fixture_getenv
// (env_on, the switches' rule (hookshot_env.h, included first), over the fixture's environment)
static bool fixture_env_on(const char* name) { const char* v = fixture_getenv(name); return v && !env_value_off(v); }
#define env_on fixture_env_on
namespace Shader { namespace Backend { namespace SPIRV { static bool mma_software = true; } } }
using VkCommandBuffer = int;
using VkImageView = int;
constexpr int VK_IMAGE_VIEW_TYPE_2D=2, VK_FORMAT_B10G11R11_UFLOAT_PACK32=7;
enum class ViewUse { Storage };
struct Desc { unsigned count=1; int format=23; };
struct Info { std::vector<Desc> texture_descriptors, image_descriptors; };
struct Stage { Info info; };
struct NvnState {};
struct Program { uint64_t key; std::vector<Stage> stages; };
struct Texture { int format=VK_FORMAT_B10G11R11_UFLOAT_PACK32; };
static Texture texture;
static struct { bool push_descriptor=false; } dev;
static bool vkCmdPushDescriptorSetKHR=true;
struct ImageInfo { VkImageView imageView; };
static struct DrawWork { std::vector<ImageInfo> imgs; } draw_work;
static DrawWork& dw() { return draw_work; }   // (vk_draw.cpp's dw is a TLS_OBJECT: the calling thread's, by a call)
static unsigned handle_value=5;
static bool resolve_image=true, upscale_result=true;
static int upscale_calls, view_calls, handle_calls;
static VkCommandBuffer forwarded_cmd;
static VkImageView forwarded_input,forwarded_output;
static Texture* forwarded_texture;
static uint32_t forwarded_level;
static uint64_t forwarded_key;
static std::string forwarded_text;
static bool forwarded_temporal;
static uint32_t handle_of(const NvnState&,const Stage&,const Desc&,uint32_t index){
    ++handle_calls;return index==0?handle_value:0;
}
static int image_format(int f){return f+100;}
static VkImageView texture_view_of(uint32_t handle,int type,ViewUse use,int format,Texture** out,uint32_t* level){
    ++view_calls;
    if(handle!=5 || type!=VK_IMAGE_VIEW_TYPE_2D || use!=ViewUse::Storage || format!=123) return 0;
    *out=resolve_image?&texture:nullptr;*level=7;return 99;
}
static bool typed_upscale(VkCommandBuffer cmd,VkImageView input,VkImageView output,Texture* out,uint32_t level,uint64_t key,const char* text,bool temporal){
    ++upscale_calls;forwarded_cmd=cmd;forwarded_input=input;forwarded_output=output;forwarded_texture=out;
    forwarded_level=level;forwarded_key=key;forwarded_text=text;forwarded_temporal=temporal;return upscale_result;
}
'''

SUFFIX = r'''
static unsigned failures=0;
static void check(bool ok,const char* what){if(!ok){++failures;fprintf(stderr,"FAIL: %s\n",what);}}
int main(int argc,char** argv){
    if(argc!=2)return 90;
    uint64_t target=0xfd029a317a9a4b94ull;bool expected=true;
    if(!strcmp(argv[1],"hardware"))Shader::Backend::SPIRV::mma_software=false,expected=false;
    else if(!strcmp(argv[1],"no-f16"))Shader::Backend::SPIRV::mma_software=false,no_f16="1";
    else if(!strcmp(argv[1],"empty"))configured_keys="",expected=false;
    else if(!strcmp(argv[1],"disabled"))configured_keys="-",expected=false;
    else if(!strcmp(argv[1],"custom"))configured_keys="0123456789abcdef",target=0x0123456789abcdefull;
    else if(!strcmp(argv[1],"prefixes"))configured_keys="abcd,fd029a";
    else if(strcmp(argv[1],"software"))return 91;
    check(taau_key(target)==expected && !taau_key(0),"existing key matcher unchanged");
    NvnState state{};Program program{target,{}};Stage stage;
    stage.info.texture_descriptors={{1,23},{2,23}};stage.info.image_descriptors={{1,23}};program.stages.push_back(stage);
    key_reads=0;
    check(!taau_stand_in(19,state,program),"capability rejection unchanged");
    check(key_reads==0,"first rejected invocation performs no key lookup after matcher priming");
    key_reads=0;
    for(int i=0;i<1000;i++)check(!taau_stand_in(19,state,program),"warm rejection unchanged");
    check(key_reads==0,"1000 warm rejected invocations perform no key lookup");
    check(handle_calls==0 && view_calls==0 && upscale_calls==0,"capability rejection avoids resource boundary calls");
    if(expected){
        dev.push_descriptor=true;dw().imgs={{11},{22},{33},{44}};
        auto rejected=[&](const char* what){key_reads=0;int prior=upscale_calls;check(!taau_stand_in(19,state,program),what);check(key_reads==0 && upscale_calls==prior,"rejected resource guard never reads/forwards keys");};
        vkCmdPushDescriptorSetKHR=false;rejected("missing push entry");vkCmdPushDescriptorSetKHR=true;
        auto stages=program.stages;program.stages.clear();rejected("empty stages");program.stages=stages;
        program.stages[0].info.texture_descriptors.clear();rejected("no textures");program.stages=stages;
        program.stages[0].info.image_descriptors.clear();rejected("no images");program.stages=stages;
        dw().imgs.pop_back();rejected("descriptor span too short");dw().imgs.push_back({44});
        dw().imgs[0].imageView=0;rejected("no input view");dw().imgs[0].imageView=11;
        dw().imgs[3].imageView=0;rejected("no output view");dw().imgs[3].imageView=44;
        handle_value=0;rejected("no handle");handle_value=5;
        resolve_image=false;rejected("no resolved image");resolve_image=true;
        texture.format=0;rejected("wrong output format");texture.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        auto accepted=[&](const char* text,bool result){
            key_reads=0;int prior=upscale_calls;upscale_result=result;
            check(taau_stand_in(19,state,program)==result,"eligible helper result forwarded");
            check(upscale_calls==prior+1 && key_reads==(configured_keys?2:1),"eligible invocation retains per-call lookup count");
            check(forwarded_cmd==19 && forwarded_input==11 && forwarded_output==44 && forwarded_texture==&texture && forwarded_level==7 && forwarded_key==target && forwarded_temporal,"eligible arguments preserved");
            check(forwarded_text==text,"current key text forwarded verbatim");
        };
        accepted(configured_keys?configured_keys:"fd029a317a9a4b94",true);
        configured_keys="later diagnostic override";accepted(configured_keys,false);
        configured_keys=nullptr;accepted("fd029a317a9a4b94",true);
    }
    printf("%s failures=%u eligible=%d\n",argv[1],failures,upscale_calls);
    return failures?1:0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT)
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--emit-only', type=Path)
    p.add_argument('--cxx-arg', action='append', default=[], help='extra compiler/linker flag (repeatable)')
    args = p.parse_args()
    cpp = fixture((args.source / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8'))
    if args.emit_only:
        with args.emit_only.open('x', encoding='utf-8') as f:
            f.write(cpp)
        print('STAGED ONLY:', args.emit_only)
        return
    with tempfile.TemporaryDirectory(prefix='taau-env-') as temp:
        folder = Path(temp)
        source, exe = folder / 'fixture.cpp', folder / 'fixture.exe'
        source.write_text(cpp, encoding='utf-8')
        if Path(args.cxx).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'):
            # (/FI, -include: env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
            command = [args.cxx, '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX', '/FI' + ENV_ON_H, str(source),
                       '/Fo' + str(folder / 'fixture.obj'), '/Fe' + str(exe), *args.cxx_arg]
        else:
            command = [args.cxx, '-std=c++17', '-include', ENV_ON_H, '-Wall', '-Wextra', '-Werror', *args.cxx_arg, str(source), '-o',
                       str(exe)]
        subprocess.run(command, cwd=folder, check=True)
        failed = False
        for case in ('software', 'hardware', 'no-f16', 'empty', 'disabled', 'custom', 'prefixes'):
            result = subprocess.run([str(exe), case], cwd=folder)
            print(case, 'exit', result.returncode, flush=True)
            failed |= result.returncode != 0
        if failed:
            raise SystemExit(1)
    print('PASS: seven key configurations; rejected lookups removed, eligible per-call forwarding preserved')


if __name__ == '__main__':
    main()
