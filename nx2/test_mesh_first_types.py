"""Check the actual first-task setup creates its required 64-bit address type.
python nx2/test_mesh_first_types.py --headers <SPIRV-Headers/include> [--cxx clang++]
No GPU or game files needed; tests setup/type prerequisites, not full task execution.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--headers", "{spirv_headers}"]

ROOT=Path(__file__).resolve().parents[1]


def block(text,marker):
    start=text.index(marker);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--headers',type=Path,required=True);p.add_argument('--cxx',default='clang++');a=p.parse_args()
    vk=ROOT/'gpu/vk';source=(vk/'eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp').read_text(encoding='utf-8')
    setup=block(source,'if (stage == Stage::Task && !runtime_info.mesh_fallback && env_on("RX_VK_MESH_FIRST")')
    fixture=r'''
#include <sirit/sirit.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
using Sirit::Id;enum class Stage { Task,Compute };
struct Program { struct { bool uses_global_memory=false; } info; };
struct EmitContext;
Id DefineInput(EmitContext&,Id,bool,spv::BuiltIn);
struct EmitContext:Sirit::Module {
    struct { bool unified_descriptor_binding=true,support_int64=true; } profile;
    struct { bool mesh_fallback=false; } runtime_info;
    Stage stage=Stage::Task;std::array<Id,2> U32{};Id U64{},draw_index{},mesh_first_block_pointer{},mesh_first_member_pointer{};
    bool wants_mesh_first=false,wants_guest_pages=false;unsigned pushes=0;
    EmitContext(bool had64):Module(0x00010600) {
        AddCapability(spv::Capability::Shader);U32[1]=TypeUInt(32);
        if(had64){AddCapability(spv::Capability::Int64);U64=TypeUInt(64);}
    }
    void DefineRescalingInputPushConstant() {
        if(!Sirit::ValidId(U64))throw std::runtime_error("first-task push block requires an initialized Int64 type");
        TypeStruct(U64);++pushes;
    }
    void Enable(const Program& program) {
''' + setup + r'''
    }
};
// Input declaration is irrelevant to type initialization; use a valid ID without introducing a shader interface.
Id DefineInput(EmitContext& ctx,Id type,bool,spv::BuiltIn){return ctx.Constant(type,0U);}
int main() {
    unsigned cases=0;const bool requested=env_on("RX_VK_MESH_FIRST");
    for(unsigned i=0;i<6;i++) {
        EmitContext ctx{i==1};const Id before=ctx.U64;
        if(i==2)ctx.stage=Stage::Compute;
        if(i==3)ctx.profile.unified_descriptor_binding=false;
        if(i==4)ctx.profile.support_int64=false;
        if(i==5)ctx.runtime_info.mesh_fallback=true;
        try{ctx.Enable(Program{});}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
        const bool enabled=requested && i<2;
        if(ctx.wants_mesh_first!=enabled || ctx.pushes!=(enabled?1:0))return 2;
        if(enabled && !Sirit::ValidId(ctx.U64))return 3;
        if(i==1 && ctx.U64.value!=before.value)return 4;
        if(!enabled && i!=1 && Sirit::ValidId(ctx.U64))return 5;
        auto words=ctx.Assemble();bool capability=false;
        for(size_t at=5;at<words.size();at+=words[at]>>16)
            if((words[at]&65535)==unsigned(spv::Op::OpCapability) && words[at+1]==unsigned(spv::Capability::Int64))capability=true;
        if(capability!=(enabled || i==1))return 6;
        ++cases;
    }
    std::printf("PASS: %u setup/type/capability cases, first-task option %s\n",cases,requested?"on":"off");
}
'''
    work=ROOT/'work';work.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mesh-first-types-',dir=work) as t:
        folder=Path(t).resolve();folder.relative_to(work.resolve());cpp=folder/'fixture.cpp';exe=folder/'fixture.exe';cpp.write_text(fixture,encoding='utf-8');sirit=vk/'sirit'
        subprocess.run([a.cxx,'-std=c++20', '-include', ENV_ON_H,'-O1','-I'+str(sirit/'include'),'-I'+str(sirit/'src'),'-I'+str(a.headers),str(cpp),*map(str,sorted((sirit/'src').rglob('*.cpp'))),'-o',str(exe)],check=True)
        env=dict(os.environ);env.pop('RX_VK_MESH_FIRST',None)
        subprocess.run([str(exe)],env=env,check=True)
        env['RX_VK_MESH_FIRST']='1';subprocess.run([str(exe)],env=env,check=True)


if __name__=='__main__':main()
