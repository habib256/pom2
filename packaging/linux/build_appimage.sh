#!/usr/bin/env bash
#
# build_appimage.sh — stage an AppDir from POM2's own CMake install rules and
# turn it into a single-file AppImage.
#
# The AppDir layout is not hand-assembled: `cmake --install` with
# CMAKE_INSTALL_PREFIX=<AppDir>/usr already produces exactly what an AppImage
# wants (usr/bin/POM2 + usr/share/POM2/{fonts,pic,roms} + the .desktop and icons
# under usr/share/{applications,icons}), because POM2's install() rules target
# the FHS layout that `pom2::resourceSearchDirs()` probes as <exe>/../share/POM2.
# That means the packaged binary resolves its assets through the SAME code path
# as an apt-installed one — no packaging-only special case to rot.
#
# Two tools, both optional-with-fallback:
#   linuxdeploy   — copies the non-system shared libs POM2 needs into usr/lib
#                   and rewrites rpaths.
#   appimagetool  — squashes the AppDir into the final .AppImage.
# Inside the release container both are pre-extracted (no FUSE on CI runners);
# POM2_APPIMAGE_TOOLS_DIR points at them. Outside, we look on PATH.
#
# Env:
#   POM2_VERSION             version string for the artifact name (default: from CMake)
#   POM2_APPIMAGE_TOOLS_DIR  dir holding linuxdeploy.AppDir/ and appimagetool.AppDir/
#   POM2_APPIMAGE_SKIP_BUILD build/ is already configured+built; only package
#   POM2_CMAKE_EXTRA_ARGS    extra configure args (e.g. -DPOM2_GLES=ON for the Pi)
#   POM2_APPIMAGE_VARIANT    name tag inserted before the arch (e.g. "raspberry",
#                            "pi400") — see the naming note at the packaging step
#   ARCH                     target arch for appimagetool (default: uname -m)
#
# Usage: packaging/linux/build_appimage.sh [<build-dir>] [<out-dir>]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${1:-build-appimage}"
OUT_DIR="${2:-dist}"
APPDIR="${BUILD_DIR}/AppDir"
ARCH="${ARCH:-$(uname -m)}"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

# ─── Dear ImGui (pinned, same source of truth as CI) ────────────────────────
if [ ! -f imgui/imgui.cpp ]; then
    log "Fetching Dear ImGui (pinned via imgui_pin.env)"
    ./setup_imgui.sh
fi

# ─── Configure + build ──────────────────────────────────────────────────────
if [ -z "${POM2_APPIMAGE_SKIP_BUILD:-}" ]; then
    log "Configuring (Release) in ${BUILD_DIR}"
    cmake -S . -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr \
          -DPOM2_ENABLE_TESTS=OFF \
          ${POM2_CMAKE_EXTRA_ARGS:-}

    log "Building"
    cmake --build "$BUILD_DIR" --parallel "${POM2_JOBS:-2}"
else
    log "POM2_APPIMAGE_SKIP_BUILD set — packaging the existing ${BUILD_DIR}"
fi

VERSION="${POM2_VERSION:-$(sed -n 's/^project(pom2_imgui VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
[ -n "$VERSION" ] || VERSION="0.0"
log "Packaging POM2 v${VERSION} (${ARCH})"

if [ "$ARCH" = "x86_64" ]; then
    python3 packaging/fujinet/stage.py --target linux-x64 --dest "$BUILD_DIR"
    python3 packaging/fujinet/stage.py --target linux-x64 \
        --dest "$BUILD_DIR" --verify
fi

# ─── Stage the AppDir via the real install rules ────────────────────────────
rm -rf "$APPDIR"
DESTDIR="$PWD/$APPDIR" cmake --install "$BUILD_DIR" >/dev/null
if [ "$ARCH" = "x86_64" ]; then
    python3 packaging/fujinet/stage.py --target linux-x64 \
        --dest "$APPDIR/usr/share/POM2" --verify
fi

# AppImage requires the desktop file + icon at the AppDir ROOT as well as in
# usr/share; appimagetool reads the root copies for the .desktop metadata.
cp packaging/POM2.desktop "$APPDIR/POM2.desktop"
cp packaging/POM2.svg     "$APPDIR/POM2.svg"
# A .DirIcon is what file managers show for the AppImage itself.
cp packaging/POM2-256.png "$APPDIR/.DirIcon"

install -m 0755 packaging/linux/AppRun "$APPDIR/AppRun"

# ─── Bundle non-system libraries ────────────────────────────────────────────
TOOLS="${POM2_APPIMAGE_TOOLS_DIR:-}"
run_tool() {   # run_tool <name> [args...] — prefers the extracted AppDir copy
    local name="$1"; shift
    if [ -n "$TOOLS" ] && [ -x "${TOOLS}/${name}.AppDir/AppRun" ]; then
        "${TOOLS}/${name}.AppDir/AppRun" "$@"
    elif command -v "$name" >/dev/null; then
        "$name" "$@"
    elif command -v "${name}-${ARCH}.AppImage" >/dev/null; then
        "${name}-${ARCH}.AppImage" "$@"
    else
        return 127
    fi
}

# pom2_headless is deployed alongside the GUI when the install rules put it
# there: the release jobs run it as their boot smoke from inside the package, so
# its own dependencies have to be resolved too. Its needs are a subset of the
# GUI's (no GLFW, no GL), but stating it costs nothing and stops a future
# dependency of the console binary from silently going unbundled.
LD_EXTRA=()
[ -x "$APPDIR/usr/bin/pom2_headless" ] && \
    LD_EXTRA+=(--executable "$APPDIR/usr/bin/pom2_headless")

if run_tool linuxdeploy --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/POM2" \
        "${LD_EXTRA[@]}" \
        --desktop-file "$APPDIR/POM2.desktop" \
        --icon-file "$APPDIR/POM2.svg" 2>"${BUILD_DIR}/linuxdeploy.log"; then
    log "linuxdeploy staged the dependency libraries"
else
    echo "ERROR: linuxdeploy unavailable or failed — see ${BUILD_DIR}/linuxdeploy.log" >&2
    exit 1
fi

# linuxdeploy re-generates AppRun; put ours back (it owns the ROM-dir seeding).
install -m 0755 packaging/linux/AppRun "$APPDIR/AppRun"

# Graphics-driver libraries must come from the HOST, never the bundle: shipping
# our libGL/libEGL would override the user's GPU driver and silently drop them
# to software rendering (or fail outright on a mismatched driver ABI).
for drv in libGL.so.1 libEGL.so.1 libGLX.so.0 libOpenGL.so.0 libGLdispatch.so.0 \
           libgbm.so.1 libdrm.so.2; do
    rm -f "$APPDIR/usr/lib/$drv"
done

# ─── Squash into the final AppImage ─────────────────────────────────────────
mkdir -p "$OUT_DIR"
# The variant tag is not cosmetic. Three aarch64 packages now come out of this
# script — the generic ARM64 one (runner glibc, desktop GL), the Raspberry Pi
# one (bookworm glibc 2.36, GLES) and the core-specific Pi 4/400 one
# (-mcpu=cortex-a72, PGO+LTO) — and the publish job flattens every artifact into
# ONE directory. Without the tag all three would be POM2-v<ver>-aarch64.AppImage
# and two of them would be silently overwritten: users would download a package
# that either refuses to start on their Pi or runs the software rasteriser.
VARIANT="${POM2_APPIMAGE_VARIANT:-}"
[ -z "$VARIANT" ] || VARIANT="${VARIANT}-"
OUTFILE="${OUT_DIR}/POM2-v${VERSION}-${VARIANT}${ARCH}.AppImage"
rm -f "$OUTFILE"

# ARCH is what appimagetool stamps into the runtime; it refuses to guess.
#
# POM2_APPIMAGE_RUNTIME lets the caller supply the runtime binary explicitly.
# The aarch64 appimagetool in AppImageKit's `continuous` release embeds a
# static-pie ET_DYN runtime, which AppImageLauncher rejects outright as
# "type -1" — so the Pi job hands it the ET_EXEC `runtime-aarch64` from the
# same release instead. x86_64 already ships ET_EXEC and needs nothing.
AT_ARGS=()
if [ -n "${POM2_APPIMAGE_RUNTIME:-}" ]; then
    AT_ARGS+=(--runtime-file "$POM2_APPIMAGE_RUNTIME")
    # Compressor: the tool's own default unless overridden. The runtime and the
    # compressor have to agree and nothing upstream checks that they do — the
    # ET_EXEC AppImageKit-12 runtime reads only xz and zlib, and an appimagetool
    # whose mksquashfs writes zstd yields an image its own runtime cannot open.
    # fetch_appimage_tools.sh therefore takes BOTH halves from release 12, whose
    # default (gzip) the runtime reads; this knob is the escape hatch if that
    # pairing ever has to change, and the self-extraction probe below is what
    # actually enforces the invariant.
    if [ -n "${POM2_APPIMAGE_COMP:-}" ]; then
        AT_ARGS+=(--comp "$POM2_APPIMAGE_COMP")
    fi
fi

if ARCH="$ARCH" run_tool appimagetool "${AT_ARGS[@]}" "$APPDIR" "$OUTFILE"; then
    log "Wrote ${OUTFILE}"
else
    echo "ERROR: appimagetool not available — AppDir left at ${APPDIR}" >&2
    exit 1
fi

# ─── Can the image open itself? ─────────────────────────────────────────────
# The runtime is embedded in the file, and the payload was compressed by a
# separate tool: nothing upstream checks that the compressor the tool chose is
# one the runtime understands. When they disagree the package is still the
# right size, the right ELF type and carries the right magic — it simply cannot
# be unpacked, by anyone, ever. Ask it here, where the answer costs a second
# and points straight at the cause, rather than discovering it in a CI verify
# step twenty minutes later or in a user's bug report.
EXTRACT_PROBE="${BUILD_DIR}/extract-probe"
rm -rf "$EXTRACT_PROBE"; mkdir -p "$EXTRACT_PROBE"
# Resolve OUTFILE to an absolute path from its own directory: the probe runs
# with a different cwd, and `$OUT_DIR` may be given either relative (CI passes
# `dist`) or absolute. Prefixing REPO_ROOT blindly breaks the absolute case.
OUTFILE_ABS="$(cd "$(dirname "$OUTFILE")" && pwd)/$(basename "$OUTFILE")"
if (cd "$EXTRACT_PROBE" && "$OUTFILE_ABS" --appimage-extract >/dev/null 2>&1) \
   && [ -x "${EXTRACT_PROBE}/squashfs-root/AppRun" ]; then
    log "Self-extraction OK (runtime and payload compressor agree)"
    rm -rf "$EXTRACT_PROBE"
else
    echo "ERROR: ${OUTFILE} cannot extract itself." >&2
    echo "       Its embedded runtime does not understand the payload's" >&2
    echo "       compression. Set POM2_APPIMAGE_COMP to one it supports" >&2
    echo "       (the pinned AppImageKit-12 ARM runtime: xz or zlib)." >&2
    exit 1
fi

ls -lh "$OUTFILE"
