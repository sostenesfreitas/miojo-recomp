"""A capture's GPU wait for its late pages is timed apart from its other work, and the 600-take log line names it.

Before, the wait sat inside "retires and stamps": with a small mirror budget (RX_VK_MIRROR_MB=1024 on an 8 GB card) that
part read 2.4-3 ms a capture, six captures a frame, and nothing said it was the replay thread waiting for the GPU (issue 995).

python -I -B tests/test_capture_wait_log.py
"""
from pathlib import Path
import re
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


class CaptureWaitLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
        cls.take = block(mem, "bool take_capture(")

    def test_the_late_page_wait_is_timed_on_its_own(self):
        late = block(self.take, "if (c->late_value && c->map)")
        wait = late.index("vk_timeline_wait(c->late_value);")
        self.assertRegex(late[:wait], r"const auto (\w+) = std::chrono::steady_clock::now\(\);")
        self.assertIn("st_tk_late +=", late[wait:])
        self.assertIn("st_tk_late_n++", late[wait:])

    def test_retires_and_stamps_start_after_the_lock(self):
        lock = self.take.index("std::lock_guard lk(pools_mu);")
        self.assertIn("st_tk_lock +=", self.take[lock:lock + 300])
        self.assertRegex(self.take, r"st_tk_stamp \+= std::chrono::duration<double, std::milli>\(tk1 - tk_locked\)")

    def test_the_log_line_names_the_wait_and_resets_it(self):
        line = re.search(r'logf\("vk: a capture taken \(ms\):.*?\);', self.take, re.S).group(0)
        self.assertIn("GPU wait for late pages", line)
        self.assertIn("pools lock", line)
        self.assertIn("st_tk_late / 600", line)
        self.assertRegex(self.take, r"st_tk_late = st_tk_lock = 0, st_tk_late_n = 0;")


if __name__ == "__main__":
    unittest.main()
