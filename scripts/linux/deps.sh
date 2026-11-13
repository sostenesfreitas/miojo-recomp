#!/bin/bash
# What the Linux AppImage carries, fetched and built into build-linux/deps (or $DEPS): each part once, kept while its
# version stays the same. Run on the oldest system the AppImage is for (CI: Ubuntu 22.04, glibc 2.35).
#
#   emsdk/upstream/   LLVM's clang and lld (the layout third_party/a64-kit/tools/toolchain.py finds through EMSDK):
#                     the player's PC compiles the game with them. The play profile (work/hookshot_pieces.prof) wants
#                     LLVM 23 or newer.
#   python/           CPython (python-build-standalone) with lz4, numpy, capstone, unicorn and zstandard: the tools
#                     (and nx2/prompt_requirements.txt's, which draw the button prompts)
#   tk/               libtcl9tk9.0.so built with Xft, with libXft and libXrender: put over python/'s, which draws only
#                     X core fonts (under XWayland one 13 px bitmap font, whatever the size asked for or the screen)
#   ffmpeg/           FFmpeg's libavcodec, libswscale, libswresample, libavutil, static and LGPL, with dav1d for OoT's
#                     AV1 movies (VP8 / VP9 too) - linked into the game
#   sdl2/             libSDL2-2.0.so.0: the window, sound and controllers, placed beside the game
#   cpm/              Vulkan-Headers, SPIRV-Headers and VulkanMemoryAllocator, in the CPM cache layout nx2/build.py reads
#   appimagetool      the AppImage packer
#   runtime-x86_64    the AppImage runtime it puts in front of the image (pinned: appimagetool's own download of the
#                     'continuous' one, at packing time, fails whenever GitHub answers 504)
set -euo pipefail
cd "$(dirname "$0")/../.."
DEPS=${DEPS:-$PWD/build-linux/deps}
DL=$DEPS/downloads
mkdir -p "$DEPS" "$DL"
JOBS=$(nproc)

LLVM_VER=23.1.5
PY_TAG=20261026
PY_VER=3.12.16
TCLTK_VER=9.0.4   # (python-build-standalone's Tcl and Tk: its tk9.0 scripts stay, beside this libtcl9tk9.0.so)
TCL_SHA256=d0aed49230bc02a65c1e0229e65f34590a4b037ec40d546f32573b467f7551ea
TK_SHA256=d7a146d2917eb8b5cc95276dbf0e3d03c7464d2b19c1675357857c989301dbb4
SDL_VER=2.32.10
FFMPEG_VER=9.0.2
DAV1D_VER=1.5.4
VK_HEADERS=v1.4.345
SPIRV_HEADERS=vulkan-sdk-1.4.341.0
VMA=v3.3.0
APPIMAGETOOL=1.9.1
APPIMAGE_RUNTIME=20251108
APPIMAGE_RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
# (the SHA-256 of each release download: a part whose download differs stops the script. Change them with the versions)
LLVM_SHA256=f72edb0b36580ece7f52e23858f00c18fad4e87395ea7ad05d7e55c39f8fc78a
PY_SHA256=ce4f6d0a6cfdb3bed6e44c2f9affd85943c76a42e25ad3a259002f31d6fb8a20
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
# (GitHub's and GitLab's tag archives (dav1d, the headers) are made on demand, and their bytes may change with the host's
# compression: these are pinned by what they unpack to, tree_is below. dav1d is linked into the game's executable, the
# headers are compiled into its renderer. Each equals a clone of its tag. Change them with the versions: the script says the
# new one when it stops)
DAV1D_TREE=01ecdd01d24e9946c5ad67837390620bc943ef521c661cebebd1d7e4263d61ea
VK_HEADERS_TREE=f420c9f8639ce3a078c8792624b292ae60858318c4741f609d8a087ddd958669
SPIRV_HEADERS_TREE=d2eb655ad74ecf31e039866232142199476ffac691a8e3cfeba90993c2de5209
VMA_TREE=ab0e5e18a0b0ba7303d0df7d3f5610e5dc99f045f8006c2404cb0e49d242d322

CURL=(curl -fL --retry 5 --retry-delay 10 --proto '=https')
# A GitHub release link (github.com/O/R/releases/download/TAG/NAME) can answer 504 for a long while: the same file is
# then fetched through the GitHub API (with $GH_TOKEN when it is set).
fetch_github_api() {   # url out
    [[ $1 =~ ^https://github\.com/([^/]+/[^/]+)/releases/download/([^/]+)/([^/]+)$ ]] || return 1
    local repo=${BASH_REMATCH[1]} tag=${BASH_REMATCH[2]} name=${BASH_REMATCH[3]} auth=() id
    [ -n "${GH_TOKEN:-}" ] && auth=(-H "Authorization: Bearer $GH_TOKEN")
    id=$("${CURL[@]}" -sS "${auth[@]}" "https://api.github.com/repos/$repo/releases/tags/$tag" |
        python3 -c 'import json,sys; print(next(a["id"] for a in json.load(sys.stdin)["assets"] if a["name"] == sys.argv[1]))' "$name") ||
        return 1
    echo "release link failed: fetching $name through the GitHub API"
    "${CURL[@]}" "${auth[@]}" -H "Accept: application/octet-stream" -o "$2" "https://api.github.com/repos/$repo/releases/assets/$id"
}
fetch() {   # url file [sha256: checked on every run, the download removed when it differs]
    if [ ! -f "$DL/$2" ]; then
        "${CURL[@]}" -o "$DL/$2.part" "$1" || fetch_github_api "$1" "$DL/$2.part"
        mv -f "$DL/$2.part" "$DL/$2"
    fi
    if [ -n "${3:-}" ] && ! echo "$3  $DL/$2" | sha256sum -c --quiet -; then
        rm -f "$DL/$2"
        echo "$2: not the expected download (SHA-256 differs)"
        exit 1
    fi
}
tree_is() {   # folder sha256 download: an unpacked archive's files by name and content; anything else in it (a link) stops too
    local got other
    other=$(find "$1" ! -type f ! -type d -print -quit)
    got=$(cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
    if [ -n "$other" ] || [ "$got" != "$2" ]; then
        rm -rf "$1" "$DL/$3"
        echo "$3: not the expected files (${other:+$other is not a file; }the SHA-256 of what it unpacks to is $got, expected $2)"
        exit 1
    fi
}
done_for() { [ -f "$DEPS/.$1" ] && [ "$(cat "$DEPS/.$1")" = "$2" ]; }
mark() { echo "$2" > "$DEPS/.$1"; }

# ---- LLVM: clang, lld and clang's own headers, nothing else ----
# (in the background, beside the parts below, which do not use it: unpacking its archive is one xz thread for about three
# minutes, while the others build with every core. Its output is printed when it is done; a failure stops the script there)
LLVM_JOB=
if ! done_for llvm "$LLVM_VER"; then
  (
    fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.xz" llvm.tar.xz "$LLVM_SHA256"
    rm -rf "$DEPS/emsdk" "$DEPS/llvm-x"
    mkdir -p "$DEPS/llvm-x"
    tar -xJf "$DL/llvm.tar.xz" -C "$DEPS/llvm-x" --strip-components=1 --wildcards \
        '*/bin/clang' '*/bin/clang++' '*/bin/clang-[0-9]*' '*/bin/lld' '*/bin/ld.lld' '*/bin/llvm-profdata' '*/lib/clang/*'
    U=$DEPS/emsdk/upstream
    mkdir -p "$U/bin" "$U/lib"
    mv "$DEPS/llvm-x/bin/"* "$U/bin/"
    mv "$DEPS/llvm-x/lib/clang" "$U/lib/clang"
    rm -rf "$DEPS/llvm-x"
    # (the runtimes: only the profile and builtins libraries; no sanitizers)
    find "$U"/lib/clang/*/lib -type f ! -name 'libclang_rt.profile*.a' ! -name 'libclang_rt.builtins*.a' -delete
    strip "$U/bin/clang-"[0-9]* "$U/bin/lld" "$U/bin/llvm-profdata" 2>/dev/null || true
    "$U/bin/clang" --version | head -1
    [ -f "$U/lib/clang/${LLVM_VER%%.*}/include/stddef.h" ] || { echo "clang's own headers are missing"; exit 1; }
    mark llvm "$LLVM_VER"
  ) > "$DEPS/llvm.log" 2>&1 &
    LLVM_JOB=$!
fi

# ---- Python and the tools' modules ----
PY_MARK="$PY_VER-$PY_TAG-$(sha256sum requirements.txt nx2/prompt_requirements.txt | sha256sum | cut -c1-12)"   # (a new pin installs again)
if ! done_for python "$PY_MARK"; then
    fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_TAG/cpython-$PY_VER+$PY_TAG-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz" python.tar.gz "$PY_SHA256"
    rm -rf "$DEPS/python"
    tar -xzf "$DL/python.tar.gz" -C "$DEPS"
    "$DEPS/python/bin/python3" -m pip install --no-cache-dir --only-binary :all: --disable-pip-version-check -q \
        -r requirements.txt
    rm -rf "$DEPS/python/lib/python3.12/test" "$DEPS/python/lib/python3.12/idlelib" "$DEPS/python/lib/python3.12/ensurepip"
    find "$DEPS/python" -name __pycache__ -prune -exec rm -rf {} +
    mark python "$PY_MARK"
fi

# ---- Tk with Xft (the builder window's fonts; Tcl is built only for Tk's configure and stub library) ----
if ! done_for tk "$TCLTK_VER"; then
    fetch "https://downloads.sourceforge.net/project/tcl/Tcl/$TCLTK_VER/tcl$TCLTK_VER-src.tar.gz" "tcl$TCLTK_VER.tar.gz"
    fetch "https://downloads.sourceforge.net/project/tcl/Tcl/$TCLTK_VER/tk$TCLTK_VER-src.tar.gz" "tk$TCLTK_VER.tar.gz"
    printf '%s  %s\n' "$TCL_SHA256" "$DL/tcl$TCLTK_VER.tar.gz" "$TK_SHA256" "$DL/tk$TCLTK_VER.tar.gz" | sha256sum -c -
    rm -rf "$DEPS/src-tcl" "$DEPS/src-tk" "$DEPS/tk"
    mkdir -p "$DEPS/src-tcl" "$DEPS/src-tk" "$DEPS/tk"
    tar -xzf "$DL/tcl$TCLTK_VER.tar.gz" -C "$DEPS/src-tcl" --strip-components=1
    tar -xzf "$DL/tk$TCLTK_VER.tar.gz" -C "$DEPS/src-tk" --strip-components=1
    (cd "$DEPS/src-tcl/unix" && ./configure --enable-shared --with-system-libtommath=no && make -j"$JOBS" binaries)
    # (no rpath: libXft and libXrender are found beside it through _tkinter's $ORIGIN/../..)
    (cd "$DEPS/src-tk/unix" && ./configure --enable-shared --with-tcl="$DEPS/src-tcl/unix" --enable-xft --disable-libcups \
        --disable-xss --disable-zipfs --disable-rpath && make -j"$JOBS" binaries)
    cp "$DEPS/src-tk/unix/libtcl9tk9.0.so" "$DEPS/tk/"
    strip --strip-unneeded "$DEPS/tk/libtcl9tk9.0.so"
    readelf -d "$DEPS/tk/libtcl9tk9.0.so" | grep -q 'libXft\.so\.2' || { echo "Tk was built without Xft"; exit 1; }
    cp -L /usr/lib/x86_64-linux-gnu/libXft.so.2 /usr/lib/x86_64-linux-gnu/libXrender.so.1 "$DEPS/tk/"
    cp "$DEPS/src-tk/license.terms" "$DEPS/tk/Tk-license.terms"
    rm -rf "$DEPS/src-tcl" "$DEPS/src-tk"
    mark tk "$TCLTK_VER"
fi

# ---- dav1d and FFmpeg (static, position independent: linked into the game's executable) ----
P=$DEPS/ffmpeg
if ! done_for ffmpeg "$FFMPEG_VER-$DAV1D_VER"; then
    rm -rf "$P" "$DEPS/src-dav1d" "$DEPS/src-ffmpeg"
    fetch "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VER/dav1d-$DAV1D_VER.tar.gz" dav1d.tar.gz
    mkdir -p "$DEPS/src-dav1d" && tar -xzf "$DL/dav1d.tar.gz" -C "$DEPS/src-dav1d" --strip-components=1
    tree_is "$DEPS/src-dav1d" "$DAV1D_TREE" dav1d.tar.gz
    (cd "$DEPS/src-dav1d" && meson setup build --prefix="$P" --libdir=lib --buildtype=release --default-library=static \
        -Db_staticpic=true -Denable_tools=false -Denable_tests=false -Denable_examples=false && ninja -C build install)
    fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" ffmpeg.tar.xz "$FFMPEG_SHA256"
    mkdir -p "$DEPS/src-ffmpeg" && tar -xJf "$DL/ffmpeg.tar.xz" -C "$DEPS/src-ffmpeg" --strip-components=1
    (cd "$DEPS/src-ffmpeg" && PKG_CONFIG_PATH="$P/lib/pkgconfig" ./configure --prefix="$P" --pkg-config-flags=--static \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-everything --disable-avformat --disable-avfilter --disable-avdevice \
        --enable-avcodec --enable-swscale --enable-swresample --enable-avutil \
        --enable-libdav1d --enable-decoder=vp8,vp9,libdav1d --enable-parser=vp8,vp9,av1 \
        && make -j"$JOBS" && make install)
    rm -rf "$DEPS/src-dav1d" "$DEPS/src-ffmpeg" "$P/share" "$P/lib/pkgconfig"
    mark ffmpeg "$FFMPEG_VER-$DAV1D_VER"
fi

# ---- SDL2 (its X11, Wayland and sound back ends load their libraries at run time) ----
if ! done_for sdl2 "$SDL_VER"; then
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" sdl2.tar.gz "$SDL_SHA256"
    rm -rf "$DEPS/src-sdl2" "$DEPS/sdl2"
    mkdir -p "$DEPS/src-sdl2" && tar -xzf "$DL/sdl2.tar.gz" -C "$DEPS/src-sdl2" --strip-components=1
    cmake -S "$DEPS/src-sdl2" -B "$DEPS/src-sdl2/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_ALSA=ON
    ninja -C "$DEPS/src-sdl2/build"
    mkdir -p "$DEPS/sdl2"
    cp -L "$DEPS/src-sdl2/build/libSDL2-2.0.so.0" "$DEPS/sdl2/libSDL2-2.0.so.0"
    strip "$DEPS/sdl2/libSDL2-2.0.so.0"
    cp "$DEPS/src-sdl2/LICENSE.txt" "$DEPS/sdl2/LICENSE.txt"
    rm -rf "$DEPS/src-sdl2"
    mark sdl2 "$SDL_VER"
fi

# ---- the Vulkan back end's headers ----
if ! done_for cpm "$VK_HEADERS-$SPIRV_HEADERS-$VMA"; then
    rm -rf "$DEPS/cpm"
    get_headers() {   # repo tag name tree
        fetch "https://github.com/$1/archive/refs/tags/$2.tar.gz" "$3-$2.tar.gz"
        mkdir -p "$DEPS/cpm/$3/$2"
        tar -xzf "$DL/$3-$2.tar.gz" -C "$DEPS/cpm/$3/$2" --strip-components=1
        tree_is "$DEPS/cpm/$3/$2" "$4" "$3-$2.tar.gz"
    }
    get_headers KhronosGroup/Vulkan-Headers "$VK_HEADERS" vulkanheaders "$VK_HEADERS_TREE"
    get_headers KhronosGroup/SPIRV-Headers "$SPIRV_HEADERS" spirv-headers "$SPIRV_HEADERS_TREE"
    get_headers GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator "$VMA" vulkanmemoryallocator "$VMA_TREE"
    mark cpm "$VK_HEADERS-$SPIRV_HEADERS-$VMA"
fi

# ---- the licenses of what the AppImage carries (Python's and its modules' are in python/) ----
if ! done_for licenses "$LLVM_VER-$FFMPEG_VER-$DAV1D_VER-$SDL_VER-$TCLTK_VER"; then
    L=$DEPS/licenses
    rm -rf "$L" && mkdir -p "$L"
    curl -fsSL -o "$L/LLVM-LICENSE.TXT" "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LLVM_VER/llvm/LICENSE.TXT"
    curl -fsSL -o "$L/FFmpeg-COPYING.LGPLv2.1" "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n$FFMPEG_VER/COPYING.LGPLv2.1"
    curl -fsSL -o "$L/FFmpeg-LICENSE.md" "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n$FFMPEG_VER/LICENSE.md"
    curl -fsSL -o "$L/dav1d-COPYING" "https://code.videolan.org/videolan/dav1d/-/raw/$DAV1D_VER/COPYING"
    cp "$DEPS/sdl2/LICENSE.txt" "$L/SDL2-LICENSE.txt"
    cp "$DEPS/tk/Tk-license.terms" "$L/Tk-license.terms"
    cp /usr/share/doc/libxft2/copyright "$L/libXft-copyright"
    cp /usr/share/doc/libxrender1/copyright "$L/libXrender-copyright"
    mark licenses "$LLVM_VER-$FFMPEG_VER-$DAV1D_VER-$SDL_VER-$TCLTK_VER"
fi

# ---- appimagetool ----
if ! done_for appimagetool "$APPIMAGETOOL"; then
    fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL/appimagetool-x86_64.AppImage" appimagetool "$APPIMAGETOOL_SHA256"
    cp "$DL/appimagetool" "$DEPS/appimagetool" && chmod +x "$DEPS/appimagetool"
    mark appimagetool "$APPIMAGETOOL"
fi
if ! done_for runtime "$APPIMAGE_RUNTIME"; then
    fetch "https://github.com/AppImage/type2-runtime/releases/download/$APPIMAGE_RUNTIME/runtime-x86_64" "runtime-x86_64-$APPIMAGE_RUNTIME"
    echo "$APPIMAGE_RUNTIME_SHA256  $DL/runtime-x86_64-$APPIMAGE_RUNTIME" | sha256sum -c -
    cp "$DL/runtime-x86_64-$APPIMAGE_RUNTIME" "$DEPS/runtime-x86_64"
    mark runtime "$APPIMAGE_RUNTIME"
fi
if [ -n "$LLVM_JOB" ]; then
    llvm_status=0
    wait "$LLVM_JOB" || llvm_status=$?
    cat "$DEPS/llvm.log"
    rm -f "$DEPS/llvm.log"
    [ "$llvm_status" = 0 ] || { echo "LLVM: failed (exit $llvm_status)"; exit 1; }
fi
echo "deps in $DEPS: $(du -sh --exclude=downloads "$DEPS" | cut -f1)"
