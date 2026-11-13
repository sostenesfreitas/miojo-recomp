#!/bin/bash
# dav1d for the Windows game, in the CPM cache layout nx2/build.py reads: <cpm>/dav1d/<version>/{include,lib/libdav1d.a}.
# The CPM cache's FFmpeg has no AV1 decoder, so without it OoT's AV1 movies go to Media Foundation, the software decoder of
# Microsoft's AV1 Video Extension (a Store download where it is missing), and stutter behind their sound (issue 2316). With
# it, host/dav1d_av1.c decodes them, as dav1d does on Linux. Built with llvm-mingw (the builder's own compiler: HOOKSHOT_MINGW),
# static, from the same dav1d release and source tree as scripts/linux/deps.sh pins. Runs on Linux (CI) or in MSYS2.
#
#   HOOKSHOT_MINGW=<llvm-mingw folder> scripts/windows/dav1d.sh [<cpm folder>, default $RX_CPM, else build-windows/cpm]
#
# Needs meson, ninja and nasm 2.15 or newer (--reproducible: no time stamps in its objects). Prints the SHA-256 of the library
# it made: the same pins, llvm-mingw and nasm give the same file.
set -euo pipefail
cd "$(dirname "$0")/../.."
MINGW=${HOOKSHOT_MINGW:?set HOOKSHOT_MINGW to an llvm-mingw folder (the ucrt-x86_64 build)}
CPM=${1:-${RX_CPM:-$PWD/build-windows/cpm}}
eval "$(grep -E '^(DAV1D_VER|DAV1D_TREE)=' scripts/linux/deps.sh)"
OUT=$CPM/dav1d/$DAV1D_VER
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for t in meson ninja nasm; do command -v "$t" >/dev/null || { echo "dav1d.sh: $t is needed"; exit 1; }; done
CC=$MINGW/bin/x86_64-w64-mingw32-clang
[ -x "$CC" ] || [ -x "$CC.exe" ] || { echo "dav1d.sh: no x86_64-w64-mingw32-clang in $MINGW/bin"; exit 1; }

curl -fL --retry 5 --retry-delay 10 --proto '=https' -o "$WORK/dav1d.tar.gz" \
    "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VER/dav1d-$DAV1D_VER.tar.gz"
mkdir -p "$WORK/src" && tar -xzf "$WORK/dav1d.tar.gz" -C "$WORK/src" --strip-components=1
# (GitLab's tag archives are made on demand: pinned by what they unpack to, as scripts/linux/deps.sh's tree_is)
other=$(find "$WORK/src" ! -type f ! -type d -print -quit)
got=$(cd "$WORK/src" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
if [ -n "$other" ] || [ "$got" != "$DAV1D_TREE" ]; then
    echo "dav1d-$DAV1D_VER.tar.gz: not the expected files (${other:+$other is not a file; }SHA-256 $got, expected $DAV1D_TREE)"
    exit 1
fi

# (no time stamps in the objects, so the same pins make the same library: clang's with -mno-incremental-linker-compatible,
# nasm's with --reproducible. nasm through a wrapper: dav1d's meson.build finds it with find_program, which keeps the
# program of a cross file's list and drops its arguments)
printf '#!/bin/sh\nexec "%s" --reproducible "$@"\n' "$(command -v nasm)" > "$WORK/nasm"
chmod +x "$WORK/nasm"
cat > "$WORK/cross.txt" <<EOF
[binaries]
c = '$CC'
ar = '$MINGW/bin/llvm-ar'
strip = '$MINGW/bin/llvm-strip'
windres = '$MINGW/bin/x86_64-w64-mingw32-windres'
nasm = '$WORK/nasm'

[built-in options]
c_args = ['-mno-incremental-linker-compatible']

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
(cd "$WORK/src" && meson setup build --cross-file "$WORK/cross.txt" --prefix="$WORK/prefix" --libdir=lib \
    --buildtype=release --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false \
    -Db_ndebug=true && ninja -C build install)

rm -rf "$OUT"
mkdir -p "$OUT/lib"
cp -r "$WORK/prefix/include" "$OUT/include"
cp "$WORK/prefix/lib/libdav1d.a" "$OUT/lib/libdav1d.a"
cp "$WORK/src/COPYING" "$OUT/COPYING"   # (BSD 2-clause: shipped with the library)
echo "dav1d $DAV1D_VER for Windows: $OUT"
sha256sum "$OUT/lib/libdav1d.a"
