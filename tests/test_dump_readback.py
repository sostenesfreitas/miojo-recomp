"""Exercise actual dump copies, host visibility and typed consumers without a GPU."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT=Path(__file__).resolve().parents[1]


def block(s,marker):
    start=s.index(marker)
    at=s.index('{',start)+1
    depth=1
    while depth:
        depth+=(s[at]=='{')-(s[at]=='}')
        at+=1
    return s[start:at]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT)
    p.add_argument('--headers',type=Path,required=True)
    p.add_argument('--cxx',default='g++')
    p.add_argument('--sanitize',action='store_true')
    a=p.parse_args()
    source=(a.source/'gpu/vk/vk_dump.cpp').read_text(encoding='utf-8')
    source='\n'.join(l for l in source.splitlines() if not l.startswith('#include'))
    texture=(a.source/'gpu/vk/vk_texture.cpp').read_text(encoding='utf-8')
    replay=(a.source/'gpu/vk/vk_replay.cpp').read_text(encoding='utf-8')
    caller='if (!burst_open && dump_pending()) wait_value(value), dump_write();'
    assert caller in replay
    fixture=(ROOT/'tests/dump_readback_test.cpp').read_text(encoding='utf-8')
    fixture=fixture.replace('/* FULL_BARRIER */',block(texture,'void full_barrier(').replace('const char* why)', 'const char* why = "test")'))
    fixture=fixture.replace('/* SOURCE */',source).replace('/* CALLER */',caller)
    with tempfile.TemporaryDirectory(prefix='dump-readback-') as tmp:
        tmp=Path(tmp)
        cpp,exe=tmp/'fixture.cpp',tmp/'fixture.exe'
        cpp.write_text(fixture,encoding='utf-8')
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else []
        subprocess.run([a.cxx,'-std=c++20', '-include', ENV_ON_H,'-O1','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',*flags,'-I'+str(a.headers),str(cpp),'-o',str(exe)],check=True)
        cases=['float','uint','sint','nan-inf','d16','d24','d32','depth-scaled','rgb9e5','ssbo','ssbo-tail','raw','stats','empty']
        for case in cases:
            for coherent,implicit in [(1,1),(1,0),(0,1),(0,0)]:
                env={k:v for k,v in os.environ.items() if not k.startswith('RX_VK_DUMP')}
                env['RX_VK_DUMP_W']='16'
                if case=='rgb9e5':env['RX_VK_DUMP_9E5']='1'
                if case=='raw':env['RX_VK_DUMP_RAW']='1'
                if case=='stats':env['RX_VK_DUMP_NOPNG']='1'
                with tempfile.TemporaryDirectory(dir=tmp) as run:
                    subprocess.run([str(exe),case,str(coherent),str(implicit)],cwd=run,env=env,check=True,timeout=15,stdout=subprocess.DEVNULL)
    print('PASS 56 actual dump readback scenarios: typed output, cache/domain controls, wait and cleanup')


if __name__=='__main__':main()
