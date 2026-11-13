"""scripts/linux/Dockerfile is the Linux AppImage's CI build environment for any distribution: the same base system and the
same system packages as .github/workflows/linux-appimage.yml. Reads both files; nothing is built.

Issue: the two lists are kept by hand. A package added to the workflow alone (a new FFmpeg or SDL2 dependency) builds
in CI and fails in the container, or builds a different AppImage there; a newer base image puts a newer glibc in the
AppImage's sysroot, and the game it builds no longer runs on the systems the release is for.

    python -I -B tests/test_linux_docker_packages.py
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCKERFILE = ROOT / "scripts/linux/Dockerfile"
WORKFLOW = ROOT / ".github/workflows/linux-appimage.yml"


def installs(text):
    """Every package an apt-get install names, with the commands' line continuations joined."""
    text = re.sub(r"\\\n", " ", text)
    packages = set()
    for args in re.findall(r"apt-get install([^&;\n]*)", text):
        packages.update(a for a in args.split() if not a.startswith("-"))
    return packages


def ppas(text):
    """Repository operands, not add-apt-repository's -y/--no-update flags."""
    text = re.sub(r"\\\n", " ", text)
    return set(re.findall(r"add-apt-repository\s+(?:-\S+\s+)*(\S+)", text))


class LinuxDockerPackages(unittest.TestCase):
    def setUp(self):
        self.docker = DOCKERFILE.read_text(encoding="utf-8")
        self.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_base_image(self):
        ci = re.search(r"^\s*container:\s*(\S+)", self.workflow, re.M)
        base = re.search(r"^FROM\s+(\S+)", self.docker, re.M)
        self.assertIsNotNone(ci, "the workflow's container")
        self.assertIsNotNone(base, "the Dockerfile's FROM")
        self.assertEqual(base.group(1), ci.group(1))

    def test_packages(self):
        ci, docker = installs(self.workflow), installs(self.docker)
        self.assertIn("g++-13", ci, "the workflow's package list was not found")
        self.assertEqual(sorted(ci - docker), [], "in the workflow, not in the Dockerfile")
        self.assertEqual(sorted(docker - ci), [], "in the Dockerfile, not in the workflow")

    def test_ppa_options(self):
        ppa = "ppa:example/testing"
        for flags in ("", "-y ", "--no-update ", "-y --no-update ", "--no-update -y "):
            with self.subTest(flags=flags):
                self.assertEqual(ppas("timeout 180s add-apt-repository " + flags + ppa + " && apt-get update"), {ppa})
        self.assertEqual(ppas("add-apt-repository -y \\\n --no-update " + ppa), {ppa})
        self.assertEqual(ppas("apt-get update"), set())

    def test_ppas(self):
        self.assertIn("ppa:ubuntu-toolchain-r/test", ppas(self.workflow), "the workflow's PPA was not found")
        self.assertEqual(ppas(self.docker), ppas(self.workflow))


if __name__ == "__main__":
    unittest.main()
