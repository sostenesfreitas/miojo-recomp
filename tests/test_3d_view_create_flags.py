"""3D images used by sampled2D views carry the enabled extension's creation flag.

python -I -B tests/test_3d_view_create_flags.py [--cxx clang++]
MSVC requires a configured developer prompt and --cxx cl. No game or GPU.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT)
    parser.add_argument('--cxx',default='clang++')
    a=parser.parse_args()
    source=(a.source/'gpu/vk/vk_texture.cpp').read_text(encoding='utf-8')
    start=source.index('    if (x.type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;') if '    if (x.type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;' in source else source.index('    if (x.type == VK_IMAGE_TYPE_3D) {', source.index('VkImageCreateFlags flags = 0;'))
    fragment=source[start:source.index('    VkImageCreateInfo ci',start)]
    fixture=r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);std::exit(1);}}while(0)
enum{VK_IMAGE_TYPE_2D=1,VK_IMAGE_TYPE_3D=2,VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT=32,VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT=128};
struct{bool view_2d_of_3d;}dev;
unsigned run(int type,bool enabled,unsigned prior){struct{int type;}x{type};dev.view_2d_of_3d=enabled;unsigned flags=prior;
"""+fragment+r"""
return flags;}
int main(){
for(unsigned prior:{0u,1u,2u,4u,7u}) {
for(bool enabled:{false,true}){
CHECK(run(VK_IMAGE_TYPE_2D,enabled,prior)==prior);
CHECK(run(VK_IMAGE_TYPE_3D,enabled,prior)==(prior|VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT|(enabled?VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT:0)));
}
}
std::puts("PASS: actual3D allocation flags preserve existing bits and gate2D-view capability");}
"""
    fixture='#include <initializer_list>\n'+fixture
    with tempfile.TemporaryDirectory(prefix='3d-view-flags-') as tmp:
        folder=Path(tmp);cpp=folder/'fixture.cpp';exe=folder/'fixture.exe';cpp.write_text(fixture,encoding='utf-8')
        if Path(a.cxx).name.lower() in ('cl','cl.exe','clang-cl','clang-cl.exe'):
            cmd=[a.cxx,'/nologo','/std:c++17','/EHsc',str(cpp),'/Fo'+str(folder/'fixture.obj'),'/Fe'+str(exe)]
        else:cmd=[a.cxx,'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)]
        subprocess.run(cmd,cwd=folder,check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
