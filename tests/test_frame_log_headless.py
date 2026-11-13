"""The frame logger emits CSV sequence numbers and elapsed/interval times, or no file when disabled.

Compiles the actual frame_log_present writer used by windowed and headless presents. Headless integration is covered
by the reported game runs; this test does not infer runtime behavior from call-site spelling or unrelated signatures."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
WITNESS = "PASS: frame log CSV header, sequence, elapsed/interval times, and disabled output"
FIXTURE = r'''
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
@@FUNCTION@@
int main() {
    for (int i = 0; i < 3; i++) {
        if (i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        frame_log_present();
    }
}
'''


def strip_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


def function(text, name, params):
    """The definition of name(params), by its name and parameter list (any spacing), from the start of its line (the
    return type, whatever it is now) to its closing brace. A declaration ends in ';' and is not taken for it."""
    tokens = r'\s*'.join(map(re.escape, re.findall(r'\w+|\S', params)))
    found = re.search(r'\b%s\s*\(\s*%s\s*\)\s*\{' % (re.escape(name), tokens), text)
    if not found:
        raise SystemExit('FAIL: no definition in gpu/vk/vk_replay.cpp: %s(%s)' % (name, params))
    start = text.rfind('\n', 0, found.start()) + 1
    pos, depth = found.end(), 1
    while pos < len(text) and depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    if depth:
        raise SystemExit('FAIL: unclosed function: %s(%s)' % (name, params))
    return text[start:pos]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cxx', default='g++')
    ap.add_argument('--source', type=Path, default=ROOT)
    args = ap.parse_args()
    text = strip_comments((args.source / 'gpu/vk/vk_replay.cpp').read_text(encoding='utf-8'))
    log = function(text, 'frame_log_present', '')
    with tempfile.TemporaryDirectory(prefix='frame-log-') as tmp:
        src, exe, csv = Path(tmp) / 'fixture.cpp', Path(tmp) / 'fixture.exe', Path(tmp) / 'frames.csv'
        src.write_text(FIXTURE.replace('@@FUNCTION@@', log), encoding='utf-8')
        result = subprocess.run([args.cxx, '-std=c++17', '-O1', str(src), '-o', str(exe)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        env = {k: v for k, v in os.environ.items() if k != 'RX_VK_FRAME_LOG'}
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
        if result.returncode or csv.exists():
            raise SystemExit('FAIL: without RX_VK_FRAME_LOG\n' + result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=dict(env, RX_VK_FRAME_LOG=str(csv)))
        if result.returncode or not csv.exists():
            raise SystemExit('FAIL: RX_VK_FRAME_LOG set, no file\n' + result.stdout + result.stderr)
        lines = csv.read_text(encoding='utf-8').splitlines()
    if lines[0] != 'frame,time_s,frametime_ms' or len(lines) != 4:
        raise SystemExit('FAIL: a header and three lines expected:\n' + '\n'.join(lines))
    rows = [line.split(',') for line in lines[1:]]
    if [int(r[0]) for r in rows] != [0, 1, 2] or float(rows[0][1]) != 0 or float(rows[0][2]) != 0:
        raise SystemExit('FAIL: frames 0, 1, 2 from time 0 expected:\n' + '\n'.join(lines))
    if not all(float(r[2]) >= 19 for r in rows[1:]) or not float(rows[1][1]) < float(rows[2][1]):
        raise SystemExit('FAIL: the frame times are not the time between the calls (20 ms):\n' + '\n'.join(lines))
    print(WITNESS)


if __name__ == '__main__':
    main()
