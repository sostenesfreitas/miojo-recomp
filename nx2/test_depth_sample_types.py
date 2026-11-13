"""Validate real texture-pass depth sampling without game files or a GPU.

python nx2/test_depth_sample_types.py --headers <SPIRV-Headers/include> --spirv-val spirv-val [--cxx clang++]
Builds the same Eden IR/SPIR-V and Sirit sources as the renderer, with its compatibility headers.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import concurrent.futures
import os
from pathlib import Path
import subprocess
import tempfile

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"]
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
RUN_ALL_SKIP = {"windows": "needs spirv-val: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]


def build_fixture(cxx, flags, sources, exe):
    """What one compiler call with every source builds, made one object a source, as many at once as there are CPUs,
    then linked: the same program, faster, and each object one a compiler cache can keep"""
    (exe.parent / 'obj').mkdir()
    objs = [exe.parent / 'obj' / ('%03d.o' % i) for i in range(len(sources))]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        list(pool.map(lambda job: subprocess.run([cxx, *flags, '-c', str(job[0]), '-o', str(job[1])], check=True),
                      zip(sources, objs)))
    subprocess.run([cxx, *map(str, objs), '-o', str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--spirv-val', default='spirv-val')
    args = parser.parse_args()
    vk = ROOT / 'gpu/vk'
    sources = sorted({p for directory in ('eden/shader_recompiler/backend/spirv',
                      'eden/shader_recompiler/frontend/ir', 'eden/shader_recompiler/ir_opt', 'sirit/src')
                      for p in (vk / directory).rglob('*.cpp')})
    flags = ['-std=c++20', '-include', ENV_ON_H, '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-DVK_NO_PROTOTYPES', '-DMKWVK_INCLUDES=2']
    for header in ('algorithm', 'cstdio', 'cstdlib', 'cstring', 'functional', 'string', 'vector'):
        flags += ['-include', header]
    flags += ['-I' + str(p) for p in (vk / 'compat', ROOT / 'gpu/compat', vk / 'eden',
              ROOT / 'gpu/third_party/suyu', vk / 'sirit/include', vk / 'sirit/src', args.headers)]
    work = ROOT / 'work'
    work.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='depth-types-', dir=work) as temp:
        folder = Path(temp).resolve()
        exe = folder / 'fixture.exe'
        build_fixture(args.cxx, flags, [ROOT / 'tests/depth_sample_types_ir.cpp', *sources], exe)
        emitted = subprocess.run([str(exe), str(folder)], check=True, capture_output=True, text=True)
        print(emitted.stdout, end='')
        for shape in ('2d', 'array'):
            floating, integer = (folder / f'{shape}-{kind}.spv' for kind in ('float', 'integer'))
            for module in (floating, integer):
                subprocess.run([args.spirv_val, '--target-env', 'vulkan1.3', str(module)], check=True)
            assert floating.read_bytes() == integer.read_bytes(), f'{shape}: irrelevant integer answer changed depth SPIR-V'
        lines = emitted.stdout.splitlines()
        assert len(lines) == 4 and all(line.endswith('integer-queries=0') for line in lines), emitted.stdout
    print('PASS: 2D/2D-array depth sampling ignores integer-format answers; all four modules validate')


if __name__ == '__main__':
    main()
