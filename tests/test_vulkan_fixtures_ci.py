"""The lavapipe job cannot pass by skipping GPU work or selecting another device.

Only authored output and a compiled device-selection function with fake device
properties are used here: no Vulkan loader, network, game or GPU is needed.
"""
import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT
CXX = "g++"
spec = importlib.util.spec_from_file_location("vulkan_fixtures", ROOT / "tests/run_vulkan_fixtures.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
IDENTITY = """VK_LAYER_KHRONOS_validation Khronos Validation Layer 1.3.275
GPU0:
    deviceName = llvmpipe (LLVM 20.1.2, 256 bits)
    driverName = llvmpipe
    apiVersion = 1.3.275
"""


def successful(fixture):
    lines = ["Device: llvmpipe (LLVM 20.1.2, 256 bits)"]
    for pattern in fixture[2]:
        lines.append(pattern.replace(r"[1-9][0-9]*", "100").replace(r"\ ", " "))
    lines.append(fixture[3])
    lines.extend(runner.ACTIVATION)
    return "\n".join(lines) + "\n"


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class Output(unittest.TestCase):
    def test_every_case_on_the_expected_device(self):
        for fixture in runner.FIXTURES:
            self.assertIn("llvmpipe", runner.check_fixture(fixture, 0, successful(fixture), "llvmpipe"))

    def test_skip_unavailable_exit77_and_failure_never_pass(self):
        fixture = runner.FIXTURES[0]
        for code, extra in ((0, "SKIP: no loader\n"), (0, "UNAVAILABLE: no device\n"),
                            (77, ""), (1, "")):
            with self.subTest(code=code, extra=extra), self.assertRaises(ValueError):
                runner.check_fixture(fixture, code, successful(fixture) + extra, "llvmpipe")

    def test_missing_wrong_or_multiple_devices_never_pass(self):
        for fixture in runner.FIXTURES:
            output = successful(fixture)
            for changed in (output.replace("llvmpipe", "NVIDIA"), "\n".join(output.splitlines()[1:]),
                            output + "GPU: NVIDIA; Vulkan 1.3.0; driver 0x123\n"):
                with self.subTest(fixture=fixture[0]), self.assertRaises(ValueError):
                    runner.check_fixture(fixture, 0, changed, "llvmpipe")

    def test_zero_partial_and_repeated_cases_never_pass(self):
        for fixture in runner.FIXTURES:
            output = successful(fixture)
            lines = output.splitlines()
            for changed in ("Device: llvmpipe\nPASS: fixture compiles (GPU execution not requested)\n",
                            "\n".join(lines[:1] + lines[2:]), output + lines[1] + "\n",
                            output.replace("100 GPU readback", "0 GPU readback") if "GPU readback" in output else ""):
                with self.subTest(fixture=fixture[0]), self.assertRaises(ValueError):
                    runner.check_fixture(fixture, 0, changed, "llvmpipe")

    def test_each_fixture_requires_actual_validation_activation(self):
        for fixture in runner.FIXTURES:
            for message in runner.ACTIVATION:
                with self.subTest(fixture=fixture[0], message=message), self.assertRaises(ValueError):
                    runner.check_fixture(fixture, 0, successful(fixture).replace(message, ""), "llvmpipe")

    def test_validation_errors_fail_even_with_all_pass_lines(self):
        for fixture in runner.FIXTURES:
            for error in ("Validation Error: [ UNASSIGNED-Test ] broken\n", "validation: broken\n",
                          "VUID-vkCmdDispatch-None-08606: broken\n"):
                with self.subTest(fixture=fixture[0], error=error), self.assertRaises(ValueError):
                    runner.check_fixture(fixture, 0, successful(fixture) + error, "llvmpipe")

    def test_copy_requires_validation_messenger(self):
        fixture = runner.FIXTURES[1]
        for state in ("unavailable", "enabled; debug utils unavailable"):
            with self.assertRaises(ValueError):
                runner.check_fixture(fixture, 0, successful(fixture).replace("validation: enabled", "validation: " + state),
                                     "llvmpipe")

    def test_identity_requires_one_software_device_and_the_layer(self):
        runner.check_identity(IDENTITY, "llvmpipe")
        for text in ("", IDENTITY.replace("llvmpipe", "NVIDIA"),
                     IDENTITY + "deviceName = Intel\n", IDENTITY.replace(runner.LAYER, "other_layer")):
            with self.assertRaises(ValueError):
                runner.check_identity(text, "llvmpipe")

    def test_main_keeps_failure_and_runs_all_three_with_explicit_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            icd, summary = Path(tmp, "lvp_icd.json"), Path(tmp, "summary")
            icd.write_text("{}", encoding="utf-8")
            responses = [(0, IDENTITY), (0, "SKIP: no device\n")]
            responses += [(0, successful(f)) for f in runner.FIXTURES[1:]]
            env = {"VK_DRIVER_FILES": str(icd), "VK_ICD_FILENAMES": str(icd), "GITHUB_STEP_SUMMARY": str(summary)}
            with patch.dict(os.environ, env), patch.object(sys, "argv", ["runner", "--headers", tmp, "--device", "llvmpipe"]), \
                    patch.object(runner, "run", side_effect=responses) as run, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(), 1)
            self.assertEqual(run.call_count, 4)
            for call, fixture in zip(run.call_args_list[1:], runner.FIXTURES):
                command, used_env = call.args
                self.assertEqual(command[-len(fixture[1]) - 1:], [*fixture[1], "llvmpipe"])
                self.assertEqual(used_env["VK_INSTANCE_LAYERS"], runner.LAYER)
                self.assertEqual(used_env["VK_LOADER_DEBUG"], "layer")
            self.assertIn("2 passed, 1 failed, 0 skipped", summary.read_text(encoding="utf-8"))


class Workflow(unittest.TestCase):
    def test_hosted_job_rules(self):
        workflow = (SOURCE_ROOT / ".github/workflows/source-regressions.yml").read_text(encoding="utf-8")
        self.assertIn("\n  vulkan-fixtures:\n", workflow)
        job = workflow.split("\n  vulkan-fixtures:\n", 1)[1].split("\n  actionlint:\n", 1)[0]
        self.assertIn("runs-on: ubuntu-24.04", job)
        self.assertIn("timeout-minutes: 5", job)
        self.assertNotIn("self-hosted", job)
        self.assertNotIn("continue-on-error", job)
        self.assertNotIn("secrets.", job)
        self.assertRegex(workflow, r"\npermissions:\n  contents: read\n")
        self.assertIn("concurrency:\n", workflow)
        self.assertNotIn("permissions:", job)  # inherits read-only
        self.assertIn("persist-credentials: false", job)
        for use in re.findall(r"uses: (\S+)", job):
            self.assertRegex(use, r"@[0-9a-f]{40}$")
        self.assertNotIn("${{", job)  # all paths come from shell environment variables
        for required in ("mesa-vulkan-drivers", "vulkan-validationlayers", "libvulkan-dev", "vulkan-tools",
                         "dpkg-query", "lvp_icd*.json", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
                         "tests/run_vulkan_fixtures.py", "--device llvmpipe"):
            self.assertIn(required, job)
        triggers = workflow.split("\npermissions:", 1)[0]
        self.assertIn("pull_request:\n    branches: [main]", triggers)
        self.assertNotIn("paths:", triggers)


class Selection(unittest.TestCase):
    def test_aspect_selector_never_falls_back(self):
        source = (SOURCE_ROOT / "tests/present_aspect_vulkan.cpp").read_text(encoding="utf-8")
        fixture = r'''
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
struct Card { const char* name; int type; };
using VkPhysicalDevice = Card*;
#define VK_NULL_HANDLE nullptr
constexpr int VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU=2;
struct VkPhysicalDeviceProperties { const char* deviceName; int deviceType; };
void vkGetPhysicalDeviceProperties(Card* card,VkPhysicalDeviceProperties* out) {
    *out={card->name,card->type};
}
''' + block(source, "std::string lower(") + "\n" + block(source, "VkPhysicalDevice pick_device(") + r'''
int main() {
    Card integrated{"Intel UHD 770",1},software{"llvmpipe (LLVM)",4},discrete{"NVIDIA RTX 4090",2};
    std::vector<Card*> cards{&integrated,&software,&discrete};
    if(pick_device(cards,"")!=&discrete || pick_device({&integrated,&software},"")!=&integrated) return 1;
    if(pick_device(cards,lower("LLVMPipe"))!=&software || pick_device(cards,"intel")!=&integrated) return 2;
    if(pick_device(cards,"missing")!=VK_NULL_HANDLE || pick_device({},"missing")!=VK_NULL_HANDLE) return 3;
    std::puts("PASS: default preference, case-insensitive selection, unmatched device refused");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp, exe = Path(tmp, "selector.cpp"), Path(tmp, "selector.exe")
            cpp.write_text(fixture, encoding="utf-8")
            subprocess.run([CXX, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
        self.assertIn("devices seen:", result.stderr)
        for name in ("Intel UHD 770", "llvmpipe (LLVM)", "NVIDIA RTX 4090"):
            self.assertIn(name, result.stderr)
        self.assertIn("PASS:", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args, rest = parser.parse_known_args()
    CXX, SOURCE_ROOT = args.cxx, args.source_root
    unittest.main(argv=[sys.argv[0], *rest])
