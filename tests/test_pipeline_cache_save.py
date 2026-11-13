"""Fault-inject the actual pipeline_cache_save using real C++ containers/threads and authored file bytes.

No Vulkan loader, GPU, game or cache input. Each case is a fresh, bounded child, including detached-worker faults.
Optional --baseline-source verifies the unmodified helper's uncaught worker/caller exceptions too.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
COMPILER = None
BASELINE = None


def save_function(path):
    source = path.read_text(encoding='utf-8')
    marker = 'void pipeline_cache_save(bool force) {'
    if source.count(marker) != 1:
        raise AssertionError('expected exactly one production cache-save function')
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class PipelineCacheSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise AssertionError('a C++20 compiler is required')
        cls.temporary = tempfile.TemporaryDirectory(prefix='hookshot-pipeline-save-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name).resolve()
        if not cls.directory.is_relative_to(Path(tempfile.gettempdir()).resolve()):
            raise AssertionError('fixture must stay in its owned temporary directory')
        cls.executables = {}
        for name, source in [('fixed', ROOT / 'gpu/vk/vk_draw.cpp'), *([('original', BASELINE)] if BASELINE else [])]:
            directory = cls.directory / name
            directory.mkdir()
            (directory / 'pipeline_cache_save.inc').write_text(save_function(source), encoding='utf-8')
            executable = directory / 'check.exe'
            command = [compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-I' + str(directory), '-I' + str(ROOT / 'gpu/vk'), str(ROOT / 'tests/pipeline_cache_save_test.cpp'), '-o', str(executable)]
            if name == 'original':
                command += ['-DCACHE_VECTOR_BASELINE']
            if os.name == 'nt':
                command += ['-D_CRT_SECURE_NO_WARNINGS']  # Match the production Windows CRT contract.
            options = {'creationflags': subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == 'nt' else {}
            result = subprocess.run(command, capture_output=True, text=True, timeout=60, **options)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[name] = executable

    def run_case(self, case, original=False):
        directory = self.directory / ('data-' + case + ('-original' if original else ''))
        directory.mkdir()
        return subprocess.run([str(self.executables['original' if original else 'fixed']), case, str(directory)],
                              capture_output=True, text=True, timeout=10)

    def test_success_threshold_and_forced_completion(self):
        self.check_case('success')
        if BASELINE:
            result = self.run_case('success', original=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_forced_vector_allocation_failure(self):
        self.check_case('force-vector')

    def test_detached_vector_allocation_failure(self):
        self.check_case('async-vector')

    def test_oversize_vector_length_error(self):
        self.check_case('force-length')

    def test_forced_temporary_path_allocation_failure(self):
        self.check_case('force-string')

    def test_detached_temporary_path_allocation_failure(self):
        self.check_case('async-string')

    def test_actual_thread_state_allocation_failure(self):
        self.check_case('thread-start')

    def test_vulkan_empty_and_error_returns(self):
        self.check_case('api-results')

    def test_actual_returned_size_controls_publication(self):
        self.check_case("actual-size")

    def test_failed_write_preserves_existing_file(self):
        self.check_case('short-write')

    def test_forced_wait_preserves_existing_single_waiter_behavior(self):
        self.check_case('forced-wait')

    def test_unchanged_data_is_not_written_again(self):
        # The precompile's pipelines, all found in the cache, count towards a save: data of the size read or last written
        # is not fetched nor written again; a write that did not land is tried again.
        self.check_case('unchanged')
        if BASELINE:
            result = self.run_case('unchanged', original=True)
            self.assertNotEqual(result.returncode, 0, 'the original rewrites unchanged data')
            self.assertIn('FAIL line', result.stderr)

    def test_in_play_the_cache_is_written_only_once_it_has_grown(self):
        # With the play files, a save in play leaves the cache's own file alone for a little growth (an ask answered
        # from the driver's own cache is copied into it); past 64 MB it is written, and a forced save (Restart now, the
        # prepare) writes whatever changed once.
        self.check_case('in-play')

    def check_case(self, case):
        result = self.run_case(case)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_original_uncaught_exceptions(self):
        if not BASELINE:
            self.skipTest('optional original-source negative')
        direct = subprocess.run([str(self.executables['fixed']), 'terminate-control'], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(direct.returncode, 86, direct.stdout + direct.stderr)
        self.assertIn('uncaught worker exception', direct.stderr)
        control = subprocess.run([str(self.executables['fixed']), 'uncaught-thread-control'], capture_output=True,
                                 text=True, timeout=10)
        # This MSVC-target Clang/STL combination fail-fasts before the custom handler. Accept only that exact
        # independently reproduced runtime result, never an arbitrary crash, loader error or nonzero exit.
        if os.name == 'nt' and control.returncode == 0xC0000409:
            self.assertEqual(control.stderr, 'MSVC-target uncaught-thread control\n')
            worker_code, worker_reason = 0xC0000409, ''
        else:
            self.assertEqual(control.returncode, 86, control.stdout + control.stderr)
            self.assertIn('uncaught worker exception', control.stderr)
            worker_code, worker_reason = 86, 'uncaught worker exception'
        for case, code, reason in [('async-vector', worker_code, worker_reason),
                                   ('async-string', worker_code, worker_reason),
                                   ('force-vector', 87, 'uncaught caller exception'),
                                   ('force-length', 87, 'uncaught caller exception'),
                                   ('force-string', 87, 'uncaught caller exception'),
                                   ('thread-start', 87, 'uncaught caller exception')]:
            with self.subTest(case=case):
                result = self.run_case(case, original=True)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                if reason:
                    self.assertIn(reason, result.stderr)
                else:
                    self.assertEqual(result.stderr, '')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx')
    parser.add_argument('--baseline-source', type=Path)
    args, remaining = parser.parse_known_args()
    COMPILER, BASELINE = args.cxx, args.baseline_source
    unittest.main(argv=[__file__, *remaining])
