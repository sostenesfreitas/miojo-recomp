"""The hot pieces' object keys name the play profile by its contents, not its path (nx2/build.py flag_text): the path kept two
checkouts, or a build from a checkout other than the work folder's, from sharing the eight hot pieces (~10-20 minutes of
compile), while -DHOOKSHOT_PROFILE_SHA1 beside it is what the key needs of the profile.

python -I -B tests/test_profile_path_key.py
"""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "tools")]
spec = importlib.util.spec_from_file_location("build_profile_key", ROOT / "nx2" / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ProfilePathTests(unittest.TestCase):
    def test_the_profiles_path_is_not_in_the_key_and_its_contents_are(self):
        a = ["-O2", "-fprofile-instr-use=C:/one/work/hookshot_pieces.prof", "-DHOOKSHOT_PROFILE_SHA1=0123456789abcdef"]
        b = ["-O2", "-fprofile-instr-use=/home/two/work/hookshot_pieces.prof", "-DHOOKSHOT_PROFILE_SHA1=0123456789abcdef"]
        c = ["-O2", "-fprofile-instr-use=C:/one/work/hookshot_pieces.prof", "-DHOOKSHOT_PROFILE_SHA1=fedcba9876543210"]
        self.assertEqual(build.flag_text(a), build.flag_text(b))
        self.assertNotEqual(build.flag_text(a), build.flag_text(c))

    def test_the_hot_flags_name_the_profiles_contents_wherever_they_name_its_path(self):
        src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
        uses = [line for line in src.splitlines() if '"-fprofile-instr-use=" +' in line]
        self.assertTrue(uses)
        self.assertTrue(all("-DHOOKSHOT_PROFILE_SHA1=" in line for line in uses), uses)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
