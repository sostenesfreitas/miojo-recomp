#!/bin/bash
# The Linux AppImage built in a container (scripts/linux/Dockerfile: Ubuntu 22.04, as CI), on any distribution:
#
#   scripts/linux/docker.sh            deps.sh, then appimage.sh -> build-linux/Hookshot-<version>-x86_64.AppImage
#   scripts/linux/docker.sh deps       deps.sh only
#   scripts/linux/docker.sh shell      a shell in the build environment
#   scripts/linux/docker.sh <command>  any other command, run in the checkout
#
# The checkout is mounted at /src and the build runs as your user, so build-linux/ belongs to you. The deps are kept
# between runs in build-linux/docker-deps and build-linux/docker-vk-work, apart from a host build's deps and vk-work. DOCKER=podman uses Podman. GH_TOKEN, JOBS, HOOKSHOT_JOBS and HOOKSHOT_VERSION are passed
# through when set.
set -euo pipefail
cd "$(dirname "$0")/../.."
DOCKER=${DOCKER:-docker}
IMAGE=${HOOKSHOT_IMAGE:-hookshot-linux-build}

"$DOCKER" build -t "$IMAGE" -f scripts/linux/Dockerfile scripts/linux

case "${1:-}" in
    "")    CMD=(bash -c 'scripts/linux/deps.sh && scripts/linux/appimage.sh') ;;
    deps)  CMD=(scripts/linux/deps.sh) ;;
    shell) CMD=(bash) ;;
    *)     CMD=("$@") ;;
esac

RUN=(run --rm --init -v "$PWD:/src" -w /src)
# (Podman keeps your user with keep-id. Rootless Docker maps the container's root to you, and any other user to a
# subordinate ID that cannot write the checkout. Docker with a root daemon needs your user named)
if "$DOCKER" --version 2>/dev/null | grep -qi podman; then
    RUN+=(--userns=keep-id)
elif "$DOCKER" info --format '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    :
else
    RUN+=(--user "$(id -u):$(id -g)")
fi
# (SELinux enforcing (Fedora, RHEL): a container reads only files labelled for containers, and the checkout is not, so
# every access to /src is denied. label=disable lifts that for this container alone; a :z mount would relabel the
# checkout on the host for good)
if [ "$(cat /sys/fs/selinux/enforce 2>/dev/null)" = 1 ]; then
    RUN+=(--security-opt label=disable)
fi
[ -t 0 ] && RUN+=(-it)
# (its own deps folder: deps.sh keeps any part already built, and parts built on the host link against the host's
# newer glibc. build-linux/AppDir and the AppImage are made again on every run, so they are shared)
# The same for the Vulkan back end's objects, which appimage.sh keeps in build-linux/vk-work and only relinks: a host
# build's need the host's glibc and libstdc++ (__isoc23_strtol, std::format). Mounted over it, as appimage.sh names it.
mkdir -p build-linux/docker-deps build-linux/docker-vk-work
RUN+=(-e DEPS=/src/build-linux/docker-deps -v "$PWD/build-linux/docker-vk-work:/src/build-linux/vk-work")
for v in GH_TOKEN JOBS HOOKSHOT_JOBS HOOKSHOT_VERSION; do
    [ -n "${!v:-}" ] && RUN+=(-e "$v")
done

exec "$DOCKER" "${RUN[@]}" "$IMAGE" "${CMD[@]}"
