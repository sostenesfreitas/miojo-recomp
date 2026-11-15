#!/bin/bash
# Game-free: a player's build started from what the AppImage carries, not from the repository. CI's AppImage job packages
# the builder but never builds a game with it, so a file the AppImage leaves out was found by every player instead: #1459
# made nx2/build.py write the menu's fonts from third_party/fonts at every build start, the AppImage does not carry that
# folder, and every Linux build stopped at LibertinusSans-Regular.ttf while CI stayed green (2026-11-10, #1567).
#
#   scripts/linux/check_player_build.sh [<Hookshot-...-x86_64.AppImage>]   (default: the newest in build-linux/)
#
# The image is extracted, as its runtime does, and run with AppRun's own environment (the AppImage's Python, compiler,
# sysroot and FFmpeg) in an otherwise empty one, so nothing of the checkout or of CI's caches stands in for what the image
# lacks:
#   - AppRun --cli --help: nx2/make_game.py, as the builder window starts it;
#   - nx2/build.py --standalone --dry-run, as make_game.py runs it: its whole start-up up to the compile, which is where
#     the game's lifted code is first needed. Every other file the compile would read must be in the image, the Vulkan back
#     end's sources among them (every player's build compiles it: the AppImage carries no built one), and every
#     third-party object the build would compile must be in its third_party_objects (the same keys: nothing to compile). Once more with
#     AMD FSR 4 (HOOKSHOT_FSR=1), which compiles it with AMD's FSR API headers too.
# Neither may write into the image's tree: on a player's PC it is read-only.
set -euo pipefail
cd "$(dirname "$0")/../.."
IMAGE=${1:-$(ls -t build-linux/Hookshot-*-x86_64.AppImage 2>/dev/null | head -1 || true)}
[ -n "$IMAGE" ] && [ -f "$IMAGE" ] || { echo "no AppImage to check (scripts/linux/appimage.sh makes one)"; exit 1; }
IMAGE=$(readlink -f "$IMAGE")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && "$IMAGE" --appimage-extract >/dev/null)
R=$TMP/squashfs-root
T=$R/usr/share/hookshot
mkdir -p "$TMP/home" "$TMP/work"
# (AppRun's environment, from AppRun itself: its lines before it starts Python, sourced with $0 naming it, so HERE is the image)
sed '/^PY=/,$d' "$R/AppRun" > "$TMP/apprun-env.sh"
in_image() {
    env -i PATH=/usr/bin:/bin HOME="$TMP/home" LANG=C.UTF-8 bash -c '. "$1"; shift; exec "$@"' "$R/AppRun" "$TMP/apprun-env.sh" "$@"
}
touch "$TMP/before"
sleep 1   # (so a file written below is newer than the stamp)

echo "== the pipeline state list (assets/pipeline_states/: nx2/make_game.py merges it beside the game)"
(cd "$T" && in_image "$R/usr/python/bin/python3" -I -B nx2/pipeline_states.py check --dir assets/pipeline_states) ||
    { echo "the AppImage carries no readable assets/pipeline_states/: a new install's first field load would be drawn white"; exit 1; }

echo "== AppRun --cli --help"
in_image "$R/AppRun" --cli --help > "$TMP/help.txt"
grep -q -- "--nup" "$TMP/help.txt" || { cat "$TMP/help.txt"; echo "nx2/make_game.py did not start"; exit 1; }

echo "== nx2/build.py --standalone --dry-run (as nx2/make_game.py runs it, with no game)"
# (-B: no __pycache__ written into the image's tree. make_game.py's other settings: nx2/make_game.py, the compile's run())
(cd "$T" && in_image env OOT_WORK="$TMP/work" RX_RELEASE=1 A64_HOT_CHUNK=1 PYTHONIOENCODING=utf-8 \
    "$R/usr/python/bin/python3" -I -B -u -X utf8 nx2/build.py --standalone --jobs 1 --dry-run) | tee "$TMP/plain.txt"
grep -q "the Vulkan back end: Vulkan " "$TMP/plain.txt" || { echo "a player's build did not compile the Vulkan back end"; exit 1; }
# (the third-party objects the image carries are the ones a player's build would compile: every key found, so none compiled)
grep -qE "^dry run: the third-party objects: ([1-9][0-9]*) of \1 in " "$TMP/plain.txt" ||
    { echo "a player's build would compile third-party code the AppImage carries compiled (nx2/third_party_objects.py)"; exit 1; }
! grep -q "with AMD FSR 4" "$TMP/plain.txt" || { echo "a build without --fsr compiled AMD FSR 4 in"; exit 1; }
[ ! -e "$R/usr/lib/hookshot/hookshot_vk.so" ] || { echo "the AppImage carries a built Vulkan back end: every player's build compiles its own"; exit 1; }

echo "== the same with AMD FSR 4 (make_game.py --fsr): the Vulkan back end compiled from the image's sources and AMD's headers"
(cd "$T" && in_image env OOT_WORK="$TMP/work-fsr" RX_RELEASE=1 A64_HOT_CHUNK=1 PYTHONIOENCODING=utf-8 HOOKSHOT_FSR=1 \
    "$R/usr/python/bin/python3" -I -B -u -X utf8 nx2/build.py --standalone --jobs 1 --dry-run) | tee "$TMP/fsr.txt"
grep -q "with AMD FSR 4" "$TMP/fsr.txt" || { echo "a build with AMD FSR 4 did not compile it into the Vulkan back end"; exit 1; }

# (Python's own __pycache__ aside: AppRun runs Python with -I, which writes it where it can, and does without where it cannot)
written=$(find "$R" -type f -newer "$TMP/before" ! -path "*/__pycache__/*" -print)
if [ -n "$written" ]; then
    echo "a player's build wrote into the AppImage's tree, which is read-only on a player's PC:"
    echo "$written" | head -20
    exit 1
fi
echo "a player's build starts from the AppImage's tree"
