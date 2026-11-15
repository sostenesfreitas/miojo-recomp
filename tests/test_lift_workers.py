"""How many processes a lift runs, and the side work where there is no fork (nx2/chunks.py file_jobs, start_side, end_side).

A spawned worker (Windows) peaks at 395 MB over a whole lift, a forked one at ~2 GB. Checked here, with the cores and the
available memory stood in:
  - forked: one process per 2 GB available; spawned: one per 0.75 GB; never more than the cores (at most 16), never none;
  - HOOKSHOT_LIFT_JOBS wins over both;
  - where there is no fork, start_side runs the side work in a thread and end_side gives its result; a side run that
    raised is done again in the lift's own thread (as a failed forked process is); one process asked for: no side run.

python -I -B tests/test_lift_workers.py
"""
import os
from pathlib import Path
import sys
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import chunks  # noqa: E402


class Case(unittest.TestCase):
    def setUp(self):
        env = mock.patch.dict(os.environ)
        env.start()
        self.addCleanup(env.stop)
        for k in ("HOOKSHOT_LIFT_JOBS", "HOOKSHOT_LIFT_NO_FORK"):
            os.environ.pop(k, None)

    def jobs(self, cores, gb, fork):
        with mock.patch.object(chunks, "_available_gb", return_value=gb), \
                mock.patch.object(chunks, "_no_fork", return_value=not fork), \
                mock.patch.object(chunks.os, "sched_getaffinity", create=True, return_value=set(range(cores))):
            return chunks.file_jobs()


class Workers(Case):
    def test_forked_one_per_2_gb(self):
        self.assertEqual(self.jobs(12, 6.0, fork=True), 3)
        self.assertEqual(self.jobs(12, 1.5, fork=True), 1)

    def test_spawned_one_per_0_75_gb(self):
        self.assertEqual(self.jobs(12, 6.0, fork=False), 8)   # (this PC's case: 2 at 2 GB a process)
        self.assertEqual(self.jobs(12, 4.0, fork=False), 5)
        self.assertEqual(self.jobs(12, 0.5, fork=False), 1)

    def test_never_more_than_the_cores_or_16(self):
        self.assertEqual(self.jobs(6, 30.0, fork=False), 6)
        self.assertEqual(self.jobs(64, 200.0, fork=True), 16)

    def test_memory_unknown_gives_the_cores(self):
        self.assertEqual(self.jobs(8, None, fork=False), 8)

    def test_lift_jobs_wins(self):
        os.environ["HOOKSHOT_LIFT_JOBS"] = "2"
        self.assertEqual(self.jobs(12, 30.0, fork=False), 2)


class SideWithoutFork(Case):
    def side(self, fn, jobs=4):
        with mock.patch.object(chunks, "_no_fork", return_value=True), \
                mock.patch.object(chunks, "emit_jobs", return_value=jobs):
            return chunks.start_side(fn)

    def test_a_thread_beside_the_lift_gives_its_result(self):
        ran_in = []

        def tables():
            ran_in.append(threading.current_thread().name)
            return ["tables written"]
        side = self.side(tables)
        self.assertIsNotNone(side)
        self.assertEqual(chunks.end_side(side, tables), ["tables written"])
        self.assertEqual(ran_in, ["lift-side"])

    def test_a_failed_side_run_is_done_again_here(self):
        calls = []

        def tables():
            calls.append(threading.current_thread().name)
            if len(calls) == 1:
                raise OSError("disk said no")
            return ["tables written"]
        side = self.side(tables)
        self.assertEqual(chunks.end_side(side, tables), ["tables written"])
        self.assertEqual(calls, ["lift-side", threading.current_thread().name])

    def test_one_process_asked_for_runs_it_after(self):
        self.assertIsNone(self.side(lambda: ["x"], jobs=1))
        self.assertEqual(chunks.end_side(None, lambda: ["x"]), ["x"])


if __name__ == "__main__":
    unittest.main()
