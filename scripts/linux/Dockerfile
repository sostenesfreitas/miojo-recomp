# The Linux AppImage's build environment, for building it on any distribution: the system CI builds it in
# (.github/workflows/linux-appimage.yml, Ubuntu 22.04) with the same packages. Ubuntu 22.04 is not a preference: the
# AppImage carries this system's C library as the game's sysroot, so the game it builds runs on glibc 2.35 and newer.
# Keep the package list in step with the workflow's "System packages" step.
#
# The image holds the system packages only. The checkout is mounted at /src when it runs, so build-linux/deps (LLVM,
# Python, FFmpeg, SDL2, headers) is kept between runs and the AppImage lands in the checkout's build-linux/.
# scripts/linux/docker.sh builds the image and runs it.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
# (libstdc++ 13, from the toolchain PPA: C++20 <format>; linked in statically)
RUN apt-get update \
 && apt-get install -y --no-install-recommends software-properties-common gpg-agent \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get install -y --no-install-recommends g++-13 libstdc++-13-dev \
 && apt-get install -y --no-install-recommends git ca-certificates curl xz-utils zstd file unzip python3 \
      build-essential gcc-12 g++-12 libstdc++-12-dev nasm meson ninja-build cmake pkg-config \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxinerama-dev \
      libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgles-dev libdrm-dev libgbm-dev \
      libpulse-dev libasound2-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libxft-dev libfontconfig1-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

# (run as the caller's user, whose home is not in the image: git and pip want a writable HOME)
ENV HOME=/tmp
# (the checkout belongs to the caller, not to the container's user under every runtime: git describe names the version)
RUN git config --system --add safe.directory /src
WORKDIR /src
CMD ["bash", "-c", "scripts/linux/deps.sh && scripts/linux/appimage.sh"]
