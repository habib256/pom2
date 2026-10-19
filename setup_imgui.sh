#!/bin/bash
set -e

echo "=== POM2 setup (Dear ImGui + GLFW) ==="

# libslirp is OPTIONAL and only powers the Uthernet I (CS8900A), whose
# guest software carries its own TCP/IP stack and therefore needs raw
# Ethernet frames bridged to the host. The Uthernet II works without it —
# its W5100 is a hardware TCP/IP stack that POM2 runs on host sockets.
# Installed here because it costs nothing; CMake degrades gracefully if
# the package is unavailable on this distro.

if [[ "$OSTYPE" == "darwin"* ]]; then
    if ! command -v brew &> /dev/null; then
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
    brew install cmake glfw pkg-config
    brew install libslirp || echo "note: libslirp unavailable — Uthernet I will have no host transport"

elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    if   command -v apt    &> /dev/null; then
        # `apt update` fails as a whole when ANY configured repository is
        # unreachable — a stale third-party PPA (deadsnakes, ondrej/php…)
        # or a proxy that 403s launchpad is enough. Under `set -e` that
        # aborted the entire setup before Dear ImGui was even cloned,
        # even though every package we need lives in the distro archive
        # and was refreshed successfully. Warn and carry on; the install
        # below still fails loudly if a package is genuinely missing.
        sudo apt update || echo "note: apt update reported errors (unreachable third-party repo?) — continuing"
        sudo apt install -y cmake libglfw3-dev pkg-config libgl1-mesa-dev g++
        sudo apt install -y libslirp-dev || echo "note: libslirp-dev unavailable — Uthernet I will have no host transport"
    elif command -v dnf    &> /dev/null; then
        sudo dnf install -y cmake glfw-devel pkgconfig mesa-libGL-devel gcc-c++
        sudo dnf install -y libslirp-devel || echo "note: libslirp-devel unavailable — Uthernet I will have no host transport"
    elif command -v pacman &> /dev/null; then
        sudo pacman -S --needed cmake glfw-x11 pkgconfig mesa gcc
        sudo pacman -S --needed libslirp || echo "note: libslirp unavailable — Uthernet I will have no host transport"
    else
        echo "Unknown package manager — install cmake, GLFW3 dev headers, and a C++17 compiler manually."
        echo "(optional: libslirp dev headers, for Uthernet I Ethernet)"
    fi
fi

# Dear ImGui — pinned commit on the `docking` branch. The pin lives in
# imgui_pin.env so setup and CI can't drift apart; see that file for why the
# docking branch is required (POM2's DockSpace) and why the commit is pinned.
# shellcheck source=imgui_pin.env
source "$(dirname "$0")/imgui_pin.env"

fetch_imgui_pin() {
    # Fetch the exact commit if the server allows it (GitHub does), which keeps
    # the clone shallow. Fall back to the branch tip and warn loudly — `docking`
    # is force-pushed on rebase, so a tip that isn't the pin means the build is
    # no longer the one we tested.
    git -C imgui fetch --depth 1 origin "$IMGUI_COMMIT" 2>/dev/null \
        && git -C imgui checkout -q FETCH_HEAD \
        && return 0
    echo "  ! could not fetch $IMGUI_COMMIT directly — falling back to origin/$IMGUI_BRANCH tip"
    git -C imgui fetch --depth 1 origin "$IMGUI_BRANCH"
    git -C imgui checkout -q FETCH_HEAD
    echo "  ! WARNING: Dear ImGui is at $(git -C imgui rev-parse HEAD), not the pinned $IMGUI_COMMIT"
}

if [ ! -d "imgui" ]; then
    echo "Cloning Dear ImGui ($IMGUI_BRANCH @ ${IMGUI_COMMIT:0:12}, v$IMGUI_VERSION)..."
    git init -q imgui
    git -C imgui remote add origin "$IMGUI_REPO"
    fetch_imgui_pin
else
    have=$(git -C imgui rev-parse HEAD 2>/dev/null || echo none)
    if [ "$have" != "$IMGUI_COMMIT" ]; then
        echo "Dear ImGui is at ${have:0:12}, pin wants ${IMGUI_COMMIT:0:12} — updating..."
        fetch_imgui_pin
    else
        echo "Dear ImGui already at the pinned commit (v$IMGUI_VERSION)."
    fi
fi

mkdir -p build
cd build
cmake ..

cat <<MSG

=== Setup complete. ===

Build:
  cd build && make -j

Run from the repo root (so roms/ probes resolve):
  ./build/POM2

ROMs:
  The repo ships the full roms/ tree, so a source build boots out of the
  box. To override a dump, replace the file in roms/ (or drop one in the
  per-user data dir — see docs/USER-GUIDE.md, Your disks and saves).
MSG
