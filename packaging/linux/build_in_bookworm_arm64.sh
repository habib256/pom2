#!/usr/bin/env bash
# build_in_bookworm_arm64.sh — the Raspberry Pi (arm64) release: COMPILE + PACKAGE.
#
# Three things differ from the x86_64 job, and all three follow from the target
# being a Pi:
#
#   1. debian:bookworm, because Raspberry Pi OS *is* Debian bookworm and an
#      AppImage never bundles glibc — its floor is the BUILD image's. Building on
#      the runner's own ubuntu-24.04-arm userland would stamp GLIBC_2.39 and the
#      package would not start on a single Pi. Bookworm's 2.36 covers Pi OS
#      bookworm + trixie and arm64 Ubuntu 24.04+.
#   2. -DPOM2_GLES=ON. Mesa's V3D caps *desktop* GL at 3.1 on Pi 4/5, so POM2's
#      default GL 3.2 core request fails outright. GLES 3.0 is what the hardware
#      actually exposes.
#   3. apt-get here rather than a pinned GHCR image. Unlike bionic, bookworm is a
#      current, supported Debian whose mirrors are healthy. Freeze it into an
#      image the day that stops being true — this script is already shaped for it.
#
# Honoured env: POM2_VERSION.
set -euxo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates git wget curl file desktop-file-utils \
    build-essential cmake pkg-config \
    zlib1g-dev \
    libglfw3-dev \
    libgles2-mesa-dev libegl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
    libslirp-dev

# The bind-mounted repo is owned by the host uid, not root.
git config --global --add safe.directory '*'

# --- AppImage tooling (aarch64 builds) --------------------------------------
#     Pinned digests + the ET_EXEC runtime rationale live in the fetch script,
#     shared with the generic ARM64 job so there is one list to update.
TOOLS=/opt/appimage-tools
ARCH=aarch64 /work/packaging/linux/fetch_appimage_tools.sh "$TOOLS"
export POM2_APPIMAGE_TOOLS_DIR="$TOOLS"
cd /work

# --- Dear ImGui (pinned COMMIT from imgui_pin.env) --------------------------
tools/fetch_imgui_pinned.sh

# --- Build POM2 on the GLES tier --------------------------------------------
#     -static-libstdc++ / -static-libgcc so the only libc-family floor is
#     glibc 2.36 — bookworm's g++ would otherwise raise it via GLIBCXX symbols.
cmake -S . -B build-appimage \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DPOM2_ENABLE_TESTS=OFF \
    -DPOM2_GLES=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build-appimage --parallel "${POM2_JOBS:-2}"

# --- Package ----------------------------------------------------------------
# `raspberry` in the name, not bare `-aarch64`: since the release also publishes
# a GENERIC aarch64 AppImage (runner glibc, desktop GL, recent ARM distros), two
# same-named files would land in the publish job's single staging directory and
# one would overwrite the other in silence — handing Pi users a package that
# will not start, or desktop-ARM users the GLES build.
export POM2_APPIMAGE_SKIP_BUILD=1
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=aarch64
export POM2_APPIMAGE_RUNTIME="$TOOLS/runtime-aarch64"
export POM2_APPIMAGE_VARIANT=raspberry
packaging/linux/build_appimage.sh build-appimage dist
