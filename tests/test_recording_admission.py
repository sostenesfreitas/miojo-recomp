"""Run actual mapped-stream/pool allocation paths against authored failures and retries."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/hookshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "hookshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT)
    p.add_argument('--headers', type=Path, required=True)
    p.add_argument('--cxx', default='g++')
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--case', default='all')
    a = p.parse_args()
    draw = (a.source / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    fixture = (ROOT / 'tests/recording_admission_test.cpp').read_text(encoding='utf-8')
    record = block(draw, 'struct RecordingRes {') + ';'
    assert 'std::vector<Stream> streams;' in record and 'std::vector<VkDescriptorPool> extra;' in record
    record = record.replace('std::vector<Stream> streams;', 'FaultVector<Stream> streams;')
    record = record.replace('std::vector<VkDescriptorPool> extra;', 'FaultVector<VkDescriptorPool> extra;')
    markers = ['bool stream_next(', 'bool stream_reserve(', 'uint64_t stream_alloc(', 'BufferRef stream_copy(']
    if 'bool stream_insert(' in draw:
        markers.insert(0, 'bool stream_insert(')
    stream = '\n'.join(block(draw, marker) for marker in markers)
    start = draw.index('    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};', draw.index('bool bind_descriptors('))
    end = draw.index('\n    return true;\n}', start) + len('\n    return true;')
    fixture = fixture.replace('/* RES */', record).replace('/* STREAM */', stream)
    fixture = fixture.replace('/* POOL */', block(draw, 'VkDescriptorPool make_pool('))
    fixture = fixture.replace('/* BEGIN */', block(draw, 'void draw_begin_recording(')).replace('/* DESCRIPTORS */', draw[start:end])
    fixture = fixture.replace('/* BANK_CACHE */', block(draw, 'struct BankCache {') + ';')
    fixture = fixture.replace('/* BANK_BRANCH */', block(block(draw, 'VkDescriptorBufferInfo uniform_range('), 'if (index == 0 || index == 2)'))
    cases = ['healthy', 'no-recording', 'initial-buffer', 'initial-map', 'initial-commit', 'pool-retry', 'parallel',
             'growth-buffer', 'growth-map', 'growth-commit', 'overflow', 'rounding', 'alignment-zero',
             'alignment-three', 'grow-off', 'reuse', 'retention', 'descriptor-pool', 'descriptor-set', 'descriptor-commit', 'descriptor-retained', 'bank-retry', 'bank-same']
    if a.case != 'all':
        assert a.case in cases
        cases = [a.case]
    with tempfile.TemporaryDirectory(prefix='recording-admission-') as tmp:
        cpp, exe = Path(tmp) / 'fixture.cpp', Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if a.sanitize else []
        subprocess.run([a.cxx, '-std=c++20', '-include', ENV_ON_H, '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', *flags,
                        '-I' + str(a.headers), str(cpp), '-o', str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith('RX_VK_')}
        for case in cases:
            subprocess.run([str(exe), case], env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
