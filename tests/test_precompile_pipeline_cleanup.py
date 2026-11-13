"""Execute the precompile graphics build/publication block with counted Vulkan handles.

Only driver pipeline creation/destruction is faked; cache ownership, locks, atomics
and delayed worker publication use real C++ objects. No game or GPU is needed.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
COMPILER = None


def production_parts():
    source = (ROOT / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    start = source.index('struct PipeEntry {')
    entry = source[start:source.index('\n};', start) + 3]
    # Extract the entire actual if(p) body, including build, first-worker
    # serialization, cache lock, lookup, publication and losing-object cleanup.
    precompile = source.index('void precompile_run(')
    start = source.index('                if (p) {', precompile)
    end = source.index('\n            }\n            pre->done++;', start)
    return entry, source[start:end]


class PrecompilePipelineCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise AssertionError('a C++20 compiler is required')
        cls.temporary = tempfile.TemporaryDirectory(prefix='hookshot-precompile-cleanup-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        entry, block = production_parts()
        (directory / 'pipe_entry.inc').write_text(entry, encoding='utf-8')
        (directory / 'precompile_graphics.inc').write_text(block, encoding='utf-8')
        cls.executable = directory / 'check.exe'
        result = subprocess.run(
            [compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
             '-I' + str(directory), str(ROOT / 'tests/precompile_pipeline_cleanup_test.cpp'),
             '-o', str(cls.executable)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_pipeline_ownership(self):
        for case in ['insert', 'ready', 'pending', 'null-insert', 'null-collision',
                     'duplicate', 'delayed-worker']:
            with self.subTest(case=case):
                result = subprocess.run([str(self.executable), case], capture_output=True,
                                        text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx')
    args, rest = parser.parse_known_args()
    COMPILER = args.cxx
    unittest.main(argv=[__file__, *rest])
