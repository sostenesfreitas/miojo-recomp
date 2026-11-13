"""host/dav1d_av1.c, the AV1 movies' decoder on Windows: every picture of a stream comes out, in order, with its pts, as the
exact NV12 of what was encoded; the end drains what dav1d holds back; after a flush the same stream decodes again. With one
thread and with several (frame threads hold pictures back, and only one dav1d_get_picture per submission keeps them busy).

python -I -B tests/test_dav1d_av1.py --cxx <compiler> --dav1d <prefix with include/dav1d/dav1d.h>

The Windows library (scripts/windows/dav1d.sh), from Linux: --cross <llvm-mingw>/bin/x86_64-w64-mingw32-clang --run wine
--dav1d <cpm>/dav1d/<version>, as the Windows builder workflow checks it.

tests/dav1d_av1/lossless_64x48.ivf is synthetic (no game data): 12 pictures of the pattern below, 64x48, encoded losslessly
with libaom so the decoded bytes are known (--make-fixture writes it again, with an FFmpeg that has libaom-av1).
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--dav1d", "{dav1d}"]
RUN_ALL_SKIP = {"windows": "the dav1d library is built for Windows by scripts/windows/dav1d.sh; checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "dav1d_av1" / "lossless_64x48.ivf"
W, H, N = 64, 48, 12


def pattern():
    """The fixture's pictures, as I420 (the same formulas as expect() in the C below)"""
    out = bytearray()
    for f in range(N):
        out += bytes((x * 3 + y * 2 + f * 5) & 0xff for y in range(H) for x in range(W))
        out += bytes((64 + x * 2 + f * 3) & 0xff for y in range(H // 2) for x in range(W // 2))
        out += bytes((192 - y * 3 + f * 2) & 0xff for y in range(H // 2) for x in range(W // 2))
    return bytes(out)


def make_fixture():
    with tempfile.TemporaryDirectory() as d:
        raw = Path(d) / "src.yuv"
        raw.write_bytes(pattern())
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s",
                        "%dx%d" % (W, H), "-r", "30", "-i", str(raw), "-c:v", "libaom-av1", "-aom-params", "lossless=1",
                        "-cpu-used", "8", "-g", "6", "-f", "ivf", str(FIXTURE)], check=True)


FIXTURE_C = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *dav1d_av1_open(int w, int h);
void dav1d_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts,
                      void (*fn)(void *, const uint8_t *, const uint8_t *, int, int, int, int64_t), void *user);
void dav1d_av1_flush(void *m);
void dav1d_av1_close(void *m);
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
enum { W = 64, H = 48, N = 12 };
static uint8_t expect(int f, int plane, int x, int y) {
    return plane == 0 ? (uint8_t)(x * 3 + y * 2 + f * 5) : plane == 1 ? (uint8_t)(64 + x * 2 + f * 3) : (uint8_t)(192 - y * 3 + f * 2);
}
typedef struct { int n; int64_t pts[64]; } Seen;
static void picture(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts) {
    Seen *s = (Seen *)user;
    CHECK(w == W && h == H && stride >= W && s->n < N);
    const int f = s->n;
    for (int r = 0; r < H; r++)
        for (int x = 0; x < W; x++) CHECK(y[r * stride + x] == expect(f, 0, x, r));
    for (int r = 0; r < H / 2; r++)
        for (int x = 0; x < W / 2; x++) {
            CHECK(uv[r * stride + 2 * x] == expect(f, 1, x, r));
            CHECK(uv[r * stride + 2 * x + 1] == expect(f, 2, x, r));
        }
    s->pts[s->n++] = pts;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *fp = fopen(argv[1], "rb");
    CHECK(fp);
    static uint8_t ivf[1 << 16];
    const size_t size = fread(ivf, 1, sizeof ivf, fp);
    fclose(fp);
    CHECK(size > 32 && !memcmp(ivf, "DKIF", 4));
    void *m = dav1d_av1_open(W, H);
    CHECK(m);
    for (int pass = 0; pass < 2; pass++) {   /* (the second after a flush: a movie started again) */
        Seen s = {0};
        int units = 0;
        for (size_t at = 32; at + 12 <= size;) {
            uint32_t n; int64_t pts;
            memcpy(&n, ivf + at, 4), memcpy(&pts, ivf + at + 4, 8);
            CHECK(at + 12 + n <= size);
            dav1d_av1_decode(m, ivf + at + 12, n, pts * 1000, picture, &s);
            at += 12 + n, units++;
        }
        CHECK(units == N);
        dav1d_av1_decode(m, NULL, 0, 0, picture, &s);   /* (the end) */
        CHECK(s.n == N);
        for (int i = 0; i < N; i++) CHECK(s.pts[i] == (int64_t)i * 1000);
        dav1d_av1_flush(m);
    }
    dav1d_av1_close(m);
    dav1d_av1_close(NULL);
    puts("ok");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="cc")
    parser.add_argument("--dav1d", default="/usr")
    parser.add_argument("--cross", help="a Windows C compiler (llvm-mingw's x86_64-w64-mingw32-clang) instead of --cxx")
    parser.add_argument("--run", default="", help="what runs the test program (wine, for --cross)")
    parser.add_argument("--make-fixture", action="store_true")
    args = parser.parse_args()
    if args.make_fixture:
        make_fixture()
    prefix = Path(args.dav1d)
    static = prefix / "lib" / "libdav1d.a"
    cc = args.cross or args.cxx
    if args.cross:
        lib = [str(static), "-static"]
    else:
        lib = [str(static), "-lpthread", "-lm"] if static.exists() and str(prefix) != "/usr" else ["-ldav1d"]
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / "t.c", Path(d) / ("t.exe" if args.cross else "t")
        src.write_text(FIXTURE_C, encoding="utf-8")
        objs = []
        for c in (src, ROOT / "host" / "dav1d_av1.c"):
            objs.append(str(Path(d) / (c.stem + ".o")))
            subprocess.run([cc, "-x", "c", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(prefix / "include"),
                            "-c", str(c), "-o", objs[-1]], check=True)
        subprocess.run([cc] + objs + ["-o", str(exe)] + lib, check=True)
        run = args.run.split() + [str(exe), str(FIXTURE)]
        for threads in ("1", "4"):
            env = dict(os.environ, HOS_DAV1D_THREADS=threads)
            env.pop("HOS_NO_DAV1D", None)
            subprocess.run(run, check=True, env=env)
        off = subprocess.run(run, env=dict(os.environ, HOS_NO_DAV1D="1"), capture_output=True, text=True)
        if off.returncode == 0 or not off.stderr.strip().endswith(" m"):   # (CHECK(m) after dav1d_av1_open)
            raise SystemExit("HOS_NO_DAV1D=1 did not leave the decoder unopened:\n" + off.stdout + off.stderr)
    print("ok")


if __name__ == "__main__":
    main()
