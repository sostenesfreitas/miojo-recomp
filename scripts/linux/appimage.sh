#!/bin/bash
# The Linux AppImage: Hookshot Builder with everything it needs to build the game on the player's PC. No game data and
# nothing made from it is in it: the game is built from the player's own copy, as on Windows.
#
#   scripts/linux/deps.sh        (first: the compiler, Python, FFmpeg, SDL2, headers into build-linux/deps)
#   scripts/linux/appimage.sh    -> build-linux/Hookshot-<version>-x86_64.AppImage and its .zsync
# The same image builds the game and, with --play, starts the one it saved.
#
# Built on the oldest system it is for (CI: Ubuntu 22.04): the sysroot it carries is that system's C library, so the
# game it builds runs on glibc 2.35 and newer. work/ (hot_funcs.txt, hot_order.txt, hookshot_pieces.prof,
# image/chunks.txt: addresses and counts from playing, no game code) is carried along when it is there.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
B=$ROOT/build-linux
DEPS=${DEPS:-$B/deps}
APPDIR=$B/AppDir
# (--long: a nightly built on a tagged commit is v2.1.0-rc.1-0-g1a2b3c4, so the updaters know it is the nightly:
# builder/linux/linux_update.py track_of)
VERSION=${HOOKSHOT_VERSION:-$(git describe --tags --long --always --match "v[0-9]*" 2>/dev/null || echo dev)}
export EMSDK=$DEPS/emsdk RX_CPM=$DEPS/cpm
PY=$DEPS/python/bin/python3

# ---- the Vulkan back end, built here once (it needs no game) ----
VKW=$B/vk-work
rm -rf "$VKW/sa/build/hookshot_vk.so"
OOT_WORK=$VKW RX_MARCH=x86-64-v3 RX_VK_ONLY=1 RX_RELEASE=1 "$PY" -I nx2/build.py --standalone --jobs "${JOBS:-$(nproc)}"
VK=$VKW/sa/build/hookshot_vk.so
[ -f "$VK" ] || { echo "hookshot_vk.so was not built"; exit 1; }

# ---- the tools' native helpers, built now: the AppImage is read-only. a64emit with its C++ runtime inside (the one this system
# builds with is newer than older systems have), and the stamp build_emit.py checks, so it is not built again ----
E=third_party/a64-kit/lift
"$EMSDK/upstream/bin/clang++" -std=c++20 -O2 -shared -fPIC -static-libstdc++ -static-libgcc -Wno-everything \
    -o "$E/liba64emit.so" "$E/a64emit.cpp"
"$PY" -I -c "import hashlib, sys; f = ['$E/a64emit.cpp', '$E/suyu/arm64_to_c.h']; open('$E/liba64emit.so.sha1', 'w').write(hashlib.sha1(b''.join(open(x, 'rb').read() for x in f)).hexdigest())"
"$PY" -I third_party/a64-kit/lift/build_emit.py
"$PY" -I -c "import sys; sys.path.insert(0, 'nx2'); import zbic; zbic.lib()"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/share/hookshot" "$APPDIR/usr/lib/hookshot"
T=$APPDIR/usr/share/hookshot
L=$APPDIR/usr/lib/hookshot

# ---- the tools (the sources the player's build compiles, and the scripts) ----
cp -a nx2 host scripts LICENSE readme.md assets "$T/"
printf '%s
' "$VERSION" > "$T/VERSION"   # (no git history in here: nx2/version.py reads the release name from it)
mkdir -p "$T/builder" "$T/gpu/render" "$T/gpu/vk" "$T/third_party"
cp -a builder/linux "$T/builder/"
cp -a gpu/render/*.h "$T/gpu/render/"
# (every gpu/vk header the host's sources include: v2.0.6 shipped without oot_cheat_items.h and no Linux build finished, issue 990.
# tests/test_appimage_headers.py checks that this covers what host/ includes)
for h in $(grep -rhoE 'gpu/vk/[A-Za-z0-9_/]+[.]h' host | sort -u); do if [ -f "$h" ]; then mkdir -p "$T/$(dirname "$h")" && cp -a "$h" "$T/$h"; fi; done
cp -a third_party/opus "$T/third_party/"
cp -a third_party/kenney_input "$T/third_party/"
cp -a third_party/kenney_input_keyboard "$T/third_party/"
# (NVIDIA DLSS, when ticked in the builder: scripts/ngx_sdk.py and the Vulkan back end's sources, compiled with HOOKSHOT_NGX_SDK)
mkdir -p "$T/scripts"
cp scripts/ngx_sdk.py "$T/scripts/"
cp -a gpu/vk gpu/compat gpu/third_party "$T/gpu/"
cp -a third_party/imgui third_party/zstd third_party/fonts "$T/third_party/"
mkdir -p "$T/third_party/zbic" "$T/third_party/a64-kit"
cp -a third_party/zbic/zbic.so third_party/zbic/LICENSE third_party/zbic/PROVENANCE.md "$T/third_party/zbic/"
cp -a third_party/a64-kit/* "$T/third_party/a64-kit/"
rm -f "$T/third_party/a64-kit/tools/lld-link.exe"
if [ -d work ]; then
    mkdir -p "$T/work/image"
    for f in hot_funcs.txt hot_order.txt hookshot_pieces.prof; do [ -f "work/$f" ] && cp "work/$f" "$T/work/"; done
    [ -f work/image/chunks.txt ] && cp work/image/chunks.txt "$T/work/image/"
fi
# (the pipeline state list's sources, assets/pipeline_states/, came with assets/ above, and are required: nx2/make_game.py merges
# them beside the game, and without them a new install's first field load is drawn white for seconds while its pipelines
# compile; v2.0.8 to v2.0.14 shipped none. scripts/linux/check_player_build.sh reads them back from the image)
[ -n "$(find "$T/assets/pipeline_states" -name "*.bin.xz" 2>/dev/null)" ] || { echo "assets/pipeline_states/ holds no pipeline state list: a release ships them (docs/development.md, \"The pipeline state list\")"; exit 1; }
find "$T" -name __pycache__ -prune -exec rm -rf {} +

# ---- what it builds with ----
cp -a "$DEPS/python" "$APPDIR/usr/python"
cp "$DEPS/tk/libtcl9tk9.0.so" "$DEPS/tk/libXft.so.2" "$DEPS/tk/libXrender.so.1" "$APPDIR/usr/python/lib/"
mkdir -p "$L/emsdk"
cp -a "$DEPS/emsdk/upstream" "$L/emsdk/"
cp -a "$DEPS/ffmpeg" "$L/ffmpeg"
cp "$VK" "$L/hookshot_vk.so"
cp "$DEPS/sdl2/libSDL2-2.0.so.0" "$L/libSDL2-2.0.so.0"
for d in "$DEPS"/cpm/*/*; do
    mkdir -p "$T/cpm/${d#"$DEPS/cpm/"}"
    cp -a "$d/include" "$T/cpm/${d#"$DEPS/cpm/"}/"
done
cp -a "$DEPS/licenses" "$T/licenses"   # (LLVM, FFmpeg, dav1d, SDL2: what this AppImage carries besides Hookshot)
# ---- the licences of the third-party parts compiled into hookshot_vk.so (tests/test_appimage_licenses.py) ----
mkdir -p "$T/licenses/imgui" "$T/licenses/zstd" "$T/licenses/libertinus"
cp third_party/imgui/LICENSE.txt third_party/imgui/PROVENANCE.md "$T/licenses/imgui/"
cp third_party/zstd/LICENSE third_party/zstd/PROVENANCE.md "$T/licenses/zstd/"
cp third_party/fonts/libertinus/OFL.txt third_party/fonts/PROVENANCE.md "$T/licenses/libertinus/"
# ---- end of the licences ----

# the sysroot: this system's C library (headers, start files, the libraries to link against) and GCC's runtime pieces
S=$L/sysroot
mkdir -p "$S/usr/lib/x86_64-linux-gnu" "$S/usr/lib/gcc/x86_64-linux-gnu" "$S/lib64"
ln -s usr/lib "$S/lib"
cp -a /usr/include "$S/usr/"
rm -rf "$S/usr/include/c++" "$S/usr/include/x86_64-linux-gnu/c++" "$S/usr/include/llvm"* "$S/usr/include/clang"*
M=/usr/lib/x86_64-linux-gnu
for f in crt1.o crti.o crtn.o Scrt1.o rcrt1.o libc.so libc.so.6 libc_nonshared.a libm.so libm.so.6 libm.a libmvec.so libmvec.so.1 \
         libmvec.a libpthread.so.0 libpthread.a libdl.so.2 libdl.a librt.so.1 librt.a libutil.a libgcc_s.so.1 ld-linux-x86-64.so.2; do
    # System aliases can have absolute targets (e.g. libmvec.so). Carry their bytes;
    # update parts refuse absolute-target symlinks, and must not use the host's library.
    [ -e "$M/$f" ] && cp -aL "$M/$f" "$S/usr/lib/x86_64-linux-gnu/" || true
done
cp -aL /lib64/ld-linux-x86-64.so.2 "$S/lib64/"
GCC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/ | sort -V | tail -1)
GV=$(basename "$GCC")
G=$S/usr/lib/gcc/x86_64-linux-gnu/$GV
mkdir -p "$G" "$S/usr/include/c++" "$S/usr/include/x86_64-linux-gnu/c++"
cp -aL "/usr/include/c++/$GV" "$S/usr/include/c++/"
cp -aL "/usr/include/x86_64-linux-gnu/c++/$GV" "$S/usr/include/x86_64-linux-gnu/c++/"
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so libstdc++.a libstdc++fs.a libstdc++exp.a; do
    [ -e "$GCC/$f" ] && cp -aL "$GCC/$f" "$G/" || true
done
# (libgcc_s.so is a linker script naming /lib/x86_64-linux-gnu/libgcc_s.so.1: found in the sysroot through lib -> usr/lib)

# ---- check: the bundled compiler builds and links a program against the sysroot, and it runs ----
cat > "$B/t.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
#include <math.h>
static void *f(void *p) { return p; }
int main(void) { pthread_t t; pthread_create(&t, 0, f, 0); pthread_join(t, 0); printf("ok %g\n", sqrt(2.0)); return 0; }
EOF
"$L/emsdk/upstream/bin/clang" --sysroot="$S" -fuse-ld=lld -O2 -march=x86-64-v3 "$B/t.c" -o "$B/t" -lpthread -lm
"$B/t"
cat > "$B/t.cpp" <<'EOF'
#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
extern "C" int t_cpp(int v) { return (int)std::format("{}", v).size(); }
EOF
"$L/emsdk/upstream/bin/clang++" --sysroot="$S" -fuse-ld=lld -std=c++20 -O2 -march=x86-64-v3 -shared -fPIC -static-libstdc++ -static-libgcc \
    "$B/t.cpp" -o "$B/t.so" -ldl -lpthread -lm

# ---- the AppDir's own files ----
cp builder/linux/AppRun "$APPDIR/AppRun"
cp builder/linux/hookshot-builder.desktop "$APPDIR/"
# (RecompLauncher reads this from inside the AppImage, at /recomplauncher.json)
cp recomplauncher.json "$APPDIR/recomplauncher.json"
cp assets/hookshot.png "$APPDIR/hookshot.png"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp assets/hookshot.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/hookshot.png"

# (the update list, beside AppRun and not inside a hashed part: hookshot-linux.txt names each part's checksum)
# (the parts are served from the linux-parts release of the repository this is built in, where
# scripts/linux/publish_parts.sh uploads them: without --base the list named NotRexed/Hookshot's release, which a fork's
# CI does not upload to, so a fork's update list named parts that were not there)
python3 scripts/linux/make_parts.py "$APPDIR" "$VERSION" "$B/parts" \
    --base "https://github.com/${GITHUB_REPOSITORY:-NotRexed/Hookshot}/releases/download/linux-parts"

# The update information AppImageUpdate and Gear Lever read: the newest .zsync of this repository's latest release (or of
# the release HOOKSHOT_UPDATE_RELEASE names). appimagetool writes it into the image and makes the .zsync beside it with
# its bundled zsyncmake, which names the AppImage by its file name, next to the .zsync on the same release.
UPDATE_REPO=${GITHUB_REPOSITORY:-NotRexed/Hookshot}
UPDATE_INFO=${HOOKSHOT_UPDATE_INFO:-"gh-releases-zsync|${UPDATE_REPO%%/*}|${UPDATE_REPO#*/}|${HOOKSHOT_UPDATE_RELEASE:-latest}|Hookshot-*-x86_64.AppImage.zsync"}
OUT=$B/Hookshot-$VERSION-x86_64.AppImage
rm -f "$OUT" "$OUT.zsync"
# (from build-linux/: appimagetool writes the .zsync into the current folder)
(cd "$B" && ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$DEPS/appimagetool" --runtime-file "$DEPS/runtime-x86_64" \
    --comp zstd -u "$UPDATE_INFO" "$APPDIR" "$(basename "$OUT")")
[ -f "$OUT.zsync" ] || { echo "appimagetool made no $OUT.zsync"; exit 1; }
echo "$OUT: $(du -h "$OUT" | cut -f1), update information: $UPDATE_INFO"
