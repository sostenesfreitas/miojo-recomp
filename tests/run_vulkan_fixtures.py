"""Run the three authored Vulkan fixtures on lavapipe, with validation and no skips.

The workflow supplies exactly one ICD in both loader variables. This is separate
from run_all.py: that suite must still work on machines without a Vulkan device.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
LAYER = "VK_LAYER_KHRONOS_validation"
ACTIVATION = ('Insert instance layer "' + LAYER + '"', 'Inserted device layer "' + LAYER + '"')
FIXTURES = (
    ("test_wb_diff_observations_gpu.py", ("--run", "--device"), (
        r"PASS: off, guest/shadow parity and feedback guards",
        r"PASS: mixed chunks, guest/shadow parity and feedback guards",
        r"PASS: reused clean result, guest/shadow parity and feedback guards",
        r"PASS: zero count, guest/shadow parity and feedback guards",
        r"PASS: 65535-wide 2D grid, guest/shadow parity and feedback guards",
    ), "PASS: authored GPU write-back observations"),
    ("test_vulkan_copy_span.py", ("--gpu", "--gpu-device"), (
        r"PASS: 24-byte production copy layout, four GPU texels correct and padding untouched",
    ), "Khronos validation: enabled"),
    ("test_present_aspect_vulkan.py", ("--device",), tuple(
        "PASS " + re.escape(name) + r": [1-9][0-9]* GPU readback pixels" for name in (
            "physical16:9", "wide matching", "maximize geometry", "wide into16:9",
            "restore geometry", "padded active crop", "invalid override",
        )), "PASS: seven offscreen GPU cases; no swapchain/window, no game assets"),
)
VALIDATION_ERROR = re.compile(r"validation error|\bVUID-|^validation:", re.I | re.M)


def check_output(returncode, output):
    if returncode:
        raise ValueError("exit %d (including 77, unavailable, is a failure)" % returncode)
    if re.search(r"^\s*(?:SKIP|UNAVAILABLE)\b", output, re.I | re.M):
        raise ValueError("a fixture was skipped or unavailable")
    if VALIDATION_ERROR.search(output):
        raise ValueError("Vulkan validation reported an error (see full output above)")


def check_identity(output, expected):
    names = re.findall(r"^\s*deviceName\s*=\s*(.+)$", output, re.M)
    if len(names) != 1 or expected.lower() not in names[0].lower():
        raise ValueError("expected only %r, saw devices %r" % (expected, names))
    if not re.search(r"^" + LAYER + r"\s", output, re.M):
        raise ValueError("Khronos validation layer is not installed")


def check_fixture(fixture, returncode, output, expected):
    check_output(returncode, output)
    for message in ACTIVATION:
        if message not in output:
            raise ValueError("validation layer was not confirmed active: " + message)
    names = re.findall(r"^(?:Device|DEVICE|GPU): (.+?)(?:; Vulkan |, api |$)", output, re.M)
    if len(names) != 1 or expected.lower() not in names[0].lower():
        raise ValueError("expected a selected %r device, saw %r" % (expected, names))
    for pattern in fixture[2]:
        if len(re.findall("^" + pattern + "$", output, re.M)) != 1:
            raise ValueError("missing or repeated executed case: " + pattern)
    if fixture[3] not in output.splitlines():
        raise ValueError("missing completion/validation record: " + fixture[3])
    return names[0]


def run(command, env):
    print("$ " + shlex.join(command), flush=True)
    try:
        result = subprocess.run(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        output = result.stdout.decode("utf-8", "replace")
        code = result.returncode
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode("utf-8", "replace") + "\nTIMEOUT after 180 seconds\n"
        code = 124
    print(output, end="" if output.endswith("\n") else "\n", flush=True)
    return code, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--device", choices=("llvmpipe",), required=True)
    args = parser.parse_args()
    start = time.monotonic()
    icd = os.environ.get("VK_DRIVER_FILES", "")
    if (not icd or icd != os.environ.get("VK_ICD_FILENAMES") or not Path(icd).is_file()
            or not re.fullmatch(r"lvp_icd(?:\.x86_64)?\.json", Path(icd).name)):
        parser.error("both VK_DRIVER_FILES and VK_ICD_FILENAMES must name the same single lavapipe ICD")
    print("ICD: " + icd, flush=True)
    rows = []
    with tempfile.TemporaryDirectory(prefix="vulkan-validation-") as tmp:
        # The two fixtures without a debug messenger otherwise need not fail on
        # validation errors. Force the layer's error log to stdout and check it.
        Path(tmp, "vk_layer_settings.txt").write_text(
            "khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG\n"
            "khronos_validation.log_filename = stdout\n"
            "khronos_validation.report_flags = error\n", encoding="utf-8")
        env = dict(os.environ, VK_INSTANCE_LAYERS=LAYER, VK_LAYER_SETTINGS_PATH=tmp, VK_LOADER_DEBUG="layer")
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        code, output = run(["vulkaninfo", "--summary"], env)
        try:
            check_output(code, output)
            check_identity(output, args.device)
        except ValueError as error:
            print("FAIL: Vulkan preflight: " + str(error), flush=True)
            return 1
        for fixture in FIXTURES:
            began = time.monotonic()
            name, flags, cases, _ = fixture
            command = [sys.executable, "-I", "-B", str(ROOT / "tests" / name),
                       "--cxx", args.cxx, "--headers", str(args.headers), *flags, args.device]
            code, output = run(command, env)
            status, note = "pass", ""
            try:
                device = check_fixture(fixture, code, output, args.device)
                note = "%d cases; %s" % (len(cases), device)
            except ValueError as error:
                status, note = "FAIL", str(error)
            seconds = time.monotonic() - began
            rows.append((status, seconds, name, note))
            print("%s %.2fs %s: %s" % rows[-1], flush=True)
    failed = sum(row[0] != "pass" for row in rows)
    heading = "%d passed, %d failed, 0 skipped in %.2f s (lavapipe, validation on)" % (
        len(rows) - failed, failed, time.monotonic() - start)
    print(heading, flush=True)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        lines = ["### Vulkan fixtures: " + heading, "", "| Result | Time | Check | Note |", "|---|---:|---|---|"]
        for status, seconds, name, note in sorted(rows, key=lambda row: -row[1]):
            note = note.replace("|", "\\|").replace("`", "'")
            lines.append("| %s | %.2f s | `%s` | %s |" % (status, seconds, name, note))
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n\n")
    return int(bool(failed))


if __name__ == "__main__":
    sys.exit(main())
