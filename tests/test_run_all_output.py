"""tests/run_all.py prints a failed check's output into a pipe whose encoding has no quotes or U+FFFD (Windows' cp1252)
and still prints its table and its summary line, instead of stopping with a UnicodeEncodeError.

A copy of run_all.py runs in a scratch repository with one check that fails printing those characters; its stdout is a
pipe with PYTHONIOENCODING=cp1252, as Windows gives one, on any OS.

python -I -B tests/test_run_all_output.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# (the check writes UTF-8 bytes itself, as a compiler does: its own stdout's encoding is not what is tested here)
FAILING = ('import sys\nsys.stdout.buffer.write("error: \\u2018x\\u2019 was not declared \\ufffd \\u2192 here\\n"'
           '.encode("utf-8"))\nsys.exit(1)\n')


class FailedOutputIntoAPipe(unittest.TestCase):
    def test_cp1252_pipe_gets_the_whole_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "tests").mkdir()
            (tmp / "nx2").mkdir()
            shutil.copy(ROOT / "tests" / "run_all.py", tmp / "tests" / "run_all.py")
            (tmp / "tests" / "test_quotes.py").write_text(FAILING, encoding="utf-8")
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "core.autocrlf=false"]
            for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "t"]):
                subprocess.run(git + cmd, cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            env = {k: v for k, v in os.environ.items() if not k.startswith(("PYTHON", "GITHUB_"))}
            env.update(PYTHONIOENCODING="cp1252", PYTHONUTF8="0")
            # (not -I, which would drop PYTHONIOENCODING: the pipe's cp1252 is the point)
            r = subprocess.run([sys.executable, "-B", str(tmp / "tests" / "run_all.py"), "--jobs", "1", "test_quotes"],
                               cwd=tmp, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        out, err = r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")
        self.assertNotIn("UnicodeEncodeError", err)     # (the runner's own traceback would go to stderr)
        self.assertEqual(r.returncode, 1, err)          # (the check failed: that is all that ends the run)
        self.assertIn("‘x’ was not declared � → here", out)   # its output, whole, as UTF-8
        self.assertIn("FAIL", out)
        self.assertRegex(out, r"\n0 passed, 1 failed, 0 skipped in \d+ s")   # the summary line comes after it


if __name__ == "__main__":
    unittest.main()
