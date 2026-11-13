"""Retry the actual failed output/flags, not the first job that shares its source.

Executes the AST-extracted compile_one and main's actual compile/retry statements, stopping before linking.
The children are authored API spies that write tiny object files; no compiler, game or memory stress is involved.
"""
import argparse
import ast
import concurrent.futures
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'nx2/build.py'


def actual_coordinator(source):
    tree = ast.parse(source.read_text(encoding='utf-8'))
    compile_one, = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'compile_one']
    main, = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'main']
    start, = [i for i, node in enumerate(main.body) if isinstance(node, ast.Assign)
              and any(isinstance(t, ast.Tuple) and [getattr(n, 'id', None) for n in t.elts] == ['t0', 'failed']
                      for t in node.targets)]
    end, = [i for i, node in enumerate(main.body) if i > start and isinstance(node, ast.If)
            and isinstance(node.test, ast.Name) and node.test.id == 'still']
    # Keep the coordinator statements unchanged. Zero only means it would proceed to linking, not a full build pass.
    wrapper = ast.parse('def run_compile_jobs():\n    return 0\n').body[0]
    wrapper.body = main.body[start:end + 1] + wrapper.body
    module = ast.fix_missing_locations(ast.Module(body=[compile_one, wrapper], type_ignores=[]))
    scope = dict(os=os, time=time, concurrent=concurrent, GATE=None, no_throttle=lambda handle: None)
    exec(compile(module, str(source), 'exec'), scope)
    return scope


class CompileRetryJobsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='hookshot-retry-jobs-')
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name).resolve()
        self.assertTrue(self.folder.is_relative_to(Path(tempfile.gettempdir()).resolve()))
        self.source = self.folder / 'shared source.cpp'
        self.source.write_text('// Authored input, never compiled.\n', encoding='ascii')
        self.jobs = [('authored-compiler-' + name, str(self.source), str(self.folder / (name + '.o')),
                      ['-DAUTHORED_' + name], ('authored headers ' + name).encode('ascii')) for name in ('A', 'B')]
        self.keys = {job[2]: str(i + 1) * 40 for i, job in enumerate(self.jobs)}
        self.scope = actual_coordinator(SOURCE)
        self.calls, self.children, self.cached = [], [], []
        self.lock = threading.Lock()
        self.cache = self.folder / 'cache'
        self.cache.mkdir()

    def run_jobs(self, statuses, counted=False):
        jobs, keys, calls, children, cached, lock = self.jobs, self.keys, self.calls, self.children, self.cached, self.lock
        attempts = {job[2]: 0 for job in jobs}

        class Child:
            def __init__(child, args, **kwargs):
                child.args = args
                child.obj = args[-1]
                with lock:
                    index = attempts[child.obj]
                    attempts[child.obj] += 1
                    children.append(args)
                choices = statuses[Path(child.obj).stem]
                child.returncode = choices[min(index, len(choices) - 1)]

            def __enter__(child):
                return child

            def __exit__(child, *args):
                return False

            def communicate(child):
                if child.returncode == 0:
                    Path(child.obj).write_bytes(('authored object ' + Path(child.obj).stem).encode('ascii'))
                return '', 'authored compiler failure' if child.returncode else ''

        actual_compile = self.scope['compile_one']
        def observed_compile(job):
            with lock:
                calls.append(job)
            return actual_compile(job)

        self.scope.update(compile_one=observed_compile, jobs=jobs, a=SimpleNamespace(jobs=2),
                          subprocess=SimpleNamespace(Popen=Child, PIPE=subprocess.PIPE,
                                                     CompletedProcess=subprocess.CompletedProcess),
                          OBJ_CACHE=str(self.cache), PLANNED={(job[1], job[2]): keys[job[2]] for job in jobs},
                          cache_put=lambda obj, key: cached.append((obj, key)), counted=counted,
                          expected={str(self.source): [11, 22]}, pending=[11, 22], eta_jobs=2,
                          eta_left=lambda pending, jobs: sum(pending) / jobs, src_size=os.path.getsize)
        log = io.StringIO()
        with redirect_stdout(log):
            result = self.scope['run_compile_jobs']()
        self.log = log.getvalue()
        facts = {'coordinator_result': result,
                 'job_calls': [Path(job[2]).name for job in calls],
                 'child_calls': [Path(args[-1]).name for args in children],
                 'objects_exist': {Path(job[2]).name: Path(job[2]).exists() for job in jobs}}
        print('retry observation: ' + json.dumps(facts, sort_keys=True), flush=True)
        # Every launched child's exact compiler, flags, source and output came from its selected job.
        for args in children:
            job, = [job for job in jobs if job[2] == args[-1]]
            self.assertEqual(args, [job[0]] + job[3] + ['-c', job[1], '-o', job[2]])
        return result

    def assert_outputs(self, names):
        for job in self.jobs:
            obj, stamp = Path(job[2]), Path(job[2] + '.sha1')
            if obj.stem in names:
                self.assertEqual(obj.read_bytes(), ('authored object ' + obj.stem).encode('ascii'))
                self.assertEqual(stamp.read_text(), self.keys[job[2]])
            else:
                self.assertFalse(obj.exists())
                self.assertFalse(stamp.exists())

    def test_failed_second_output_is_the_job_retried(self):
        self.assertEqual(self.run_jobs({'A': [0], 'B': [17, 0]}), 0)
        self.assertEqual(self.calls.count(self.jobs[0]), 1)
        self.assertEqual(self.calls.count(self.jobs[1]), 2)
        self.assert_outputs({'A', 'B'})
        self.assertCountEqual(self.cached, [(job[2], self.keys[job[2]]) for job in self.jobs])

    def test_persistent_second_output_failure_stops_before_linking(self):
        self.assertEqual(self.run_jobs({'A': [0], 'B': [17, 17]}), 1)
        self.assertEqual(self.calls.count(self.jobs[0]), 1)
        self.assertEqual(self.calls.count(self.jobs[1]), 2)
        self.assert_outputs({'A'})
        self.assertEqual(self.cached, [(self.jobs[0][2], self.keys[self.jobs[0][2]])])

    def test_both_failed_jobs_are_retried_distinctly(self):
        self.assertEqual(self.run_jobs({'A': [17, 0], 'B': [17, 0]}), 0)
        self.assertEqual(self.calls.count(self.jobs[0]), 2)
        self.assertEqual(self.calls.count(self.jobs[1]), 2)
        self.assert_outputs({'A', 'B'})

    def test_successful_jobs_are_not_retried(self):
        self.assertEqual(self.run_jobs({'A': [0], 'B': [0]}), 0)
        self.assertCountEqual(self.calls, self.jobs)
        self.assert_outputs({'A', 'B'})

    def test_counted_retry_keeps_existing_progress_and_estimates(self):
        self.assertEqual(self.run_jobs({'A': [0], 'B': [17, 0]}, counted=True), 0)
        self.assertEqual(self.calls[-1], self.jobs[1])
        self.assertEqual(self.scope['pending'], [])
        self.assertEqual(self.scope['expected'][str(self.source)], [])
        records = [line for line in self.log.splitlines() if line.startswith('@@file ')]
        self.assertEqual(len(records), 3)
        self.assertEqual(sum(line.endswith(' failed') for line in records), 1)
        self.assertEqual(sum(line.endswith(' again') for line in records), 1)
        self.assert_outputs({'A', 'B'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    args, remaining = parser.parse_known_args()
    SOURCE = args.source.resolve()
    unittest.main(argv=[__file__, *remaining])
