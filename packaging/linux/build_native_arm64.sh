#!/usr/bin/env bash
# build_native_arm64.sh — the GENERIC Linux ARM64 release: COMPILE + PACKAGE.
#
# The sibling of build_in_bookworm_arm64.sh: the SAME pinned debian:bookworm
# container on the native arm64 runner (release.yml), so the same glibc 2.36
# floor — the floor POM1's aarch64 AppImage has. It used to build on the
# runner's own ubuntu-24.04-arm userland, which stamped GLIBC_2.39 and left
# out every ARM distro older than Ubuntu 24.04 (Debian 12, Ubuntu 22.04, …);
# an AppImage never bundles glibc, so an old floor is what makes it run
# everywhere (2026-11-16). What still differs from the Pi build:
#
#   · Desktop OpenGL, not GLES. An ARM64 laptop or server has a normal GL
#     driver; the GLES tier exists for the Pi's V3D, which caps desktop GL at
#     3.1 and cannot serve POM2's 3.2 core request.
#   · Bare `-aarch64` in the name. The Pi packages carry `raspberry`/`pi400`
#     tags, so the three never collide in the publish job's staging directory.
#
# A user on Pi OS wants the `raspberry` package (glibc 2.36, GLES); a user on a
# recent ARM desktop wants this one. The names say which.
#
# -static-libstdc++ / -static-libgcc for the same reason as the Pi build: an
# AppImage never bundles glibc's family, and linuxdeploy blacklists
# libstdc++.so.6 outright, so a dynamic GLIBCXX dependency would be an
# unsatisfiable requirement on any host older than the runner.
#
# Honoured env: POM2_VERSION, POM2_JOBS, POM2_INSTALL_DEPS (=1 inside the
# bookworm container: install the build dependencies with apt first).
# Usage: packaging/linux/build_native_arm64.sh
set -euxo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

if [ "${POM2_INSTALL_DEPS:-0}" = 1 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        ca-certificates git wget curl file desktop-file-utils \
        build-essential cmake pkg-config \
        zlib1g-dev \
        libglfw3-dev libgl1-mesa-dev \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
        libslirp-dev
    # The bind-mounted repo is owned by the host uid, not root.
    git config --global --add safe.directory '*'
fi

# --- AppImage tooling -------------------------------------------------------
# Same pinned digests as the Pi job (shared script). Staged inside the work
# tree rather than /opt so the job needs no root.
TOOLS="$REPO_ROOT/build-appimage-tools"
ARCH=aarch64 packaging/linux/fetch_appimage_tools.sh "$TOOLS"
export POM2_APPIMAGE_TOOLS_DIR="$TOOLS"

# --- Dear ImGui (pinned COMMIT from imgui_pin.env) --------------------------
tools/fetch_imgui_pinned.sh

# --- Build ------------------------------------------------------------------
cmake -S . -B build-appimage \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DPOM2_ENABLE_TESTS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build-appimage --parallel "${POM2_JOBS:-2}"
# The GUI binary MUST be there: if GLFW were missing, CMake would quietly leave
# us with the headless targets only, and the AppImage would be packaged around
# a hole.
test -x build-appimage/POM2

# --- Package ----------------------------------------------------------------
export POM2_APPIMAGE_SKIP_BUILD=1
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=aarch64
export POM2_APPIMAGE_RUNTIME="$TOOLS/runtime-aarch64"
# No POM2_APPIMAGE_VARIANT: this is the plain `-aarch64` package.
packaging/linux/build_appimage.sh build-appimage dist
