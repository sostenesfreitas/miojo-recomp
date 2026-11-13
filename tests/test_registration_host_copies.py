"""A texture or sampler registration kept from before the renderer started is copied from the host's copy of it.

host/rx_backend.c keeps the registrations the game makes before the window's thread starts the renderer and hands them
over as pointers to its own copies; gpu/vk/vk_bank.cpp's descriptor_ok says which memory a registration is copied from.
It used to ask the host whether the pointer was a GPU address first. With the native NVN layer every address from 16 MB
to 512 GB is one (host/nvn_native.c nvn_gpu_guest), so a copy on a Windows heap there was read from untouched guest
memory instead: all zero (issue 1117: sampler 258 filtered nearest, texture slot 0x102 had no image).

Runs descriptor_ok, cut from the source, against nvn_gpu_guest, cut from the source, with the guest window's real
bounds."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

CASES = ["guest", "copy-in-guest-address-range", "copy-at-virtual-pool-address", "copy-above-the-window", "nothing-there",
         "not-readable", "short"]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def define(source, name):
    """The literal a header gives a name (HOS_SPACE_END has a second, web-only definition that is a variable)"""
    found = re.findall(r"^#define %s (\(?[0-9][0-9A-Za-z<> ]*\)?)" % name, source, re.M)
    assert len(found) == 1, (name, found)
    return "#define %s %s" % (name, found[0])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    read = lambda path: (a.source / path).read_text(encoding="utf-8")
    bank, native, hos, a64 = (read(path) for path in ["gpu/vk/vk_bank.cpp", "host/nvn_native.c", "host/hos.h",
                                                       "third_party/a64-kit/rt/a64.h"])
    constants = "\n".join([define(hos, "HOS_SYSTEM_BASE"), define(hos, "HOS_SPACE_END"), define(hos, "HOS_GUEST_SPACE"),
                           define(native, "VBASE"), define(native, "VPAGE"), define(a64, "A64_HOST_BASE")])
    # (the registrations' entries all go through descriptor_ok: one that copied from its pointer unchecked would not)
    replay = read("gpu/vk/vk_replay.cpp")
    for source, marker in [(bank, "void register_texture(int id"), (bank, "void register_sampler(int id"),
                           (replay, "void register_texture_queued(int id"), (replay, "void register_sampler_queued(int id")]:
        assert "descriptor_ok(" in block(source, marker), marker
    fixture = (ROOT / "tests/registration_host_copies_test.cpp").read_text(encoding="utf-8")
    for marker, text in [("/* CONSTANTS */", constants), ("/* NVN_GPU_GUEST */", block(native, "uint64_t nvn_gpu_guest(uint64_t gpu) {")),
                         ("/* DESCRIPTOR_OK */", block(bank, "const void* descriptor_ok(const void* p, size_t n) {"))]:
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, text)
    with tempfile.TemporaryDirectory(prefix="registration-host-copies-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=20)
    print(f"{len(CASES)} registration pointer cases passed")


if __name__ == "__main__":
    main()
