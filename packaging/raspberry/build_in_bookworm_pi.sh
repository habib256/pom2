#!/usr/bin/env bash
# =============================================================================
#  build_in_bookworm_pi.sh — the CORE-SPECIFIC Raspberry Pi build (PGO + LTO),
#  meant to run INSIDE a debian:bookworm arm64 container with the repo bind-
#  mounted on /work:
#
#    docker run --rm -v "$PWD":/work -w /work -e POM2_MCPU=cortex-a72 \
#        debian:bookworm bash /work/packaging/raspberry/build_in_bookworm_pi.sh
#
#  HOW THIS DIFFERS from `packaging/linux/build_in_bookworm_arm64.sh` (the
#  release AppImage), and why a second script exists at all:
#
#   · `-mcpu=<the exact core>` instead of generic aarch64. The published
#     AppImage has to run from a Pi 3 to a Pi 5; a machine you are setting up
#     runs on ITS Pi only. POM2's hot loop is the 6502 interpreter plus the
#     Disk II LSS — branch-dense code that cares about the right cost model.
#   · TWO PASSES guided by profile (PGO), then LTO. Measured on x86-64 at
#     identical sources: −39 % on a CPU-bound load, −29 % on a disk-active one,
#     with byte-identical output. It is the biggest lever available without
#     touching a line of emulation, and it is FREE here — the training run
#     happens on the CI runner, so the Pi pays nothing for it.
#     → docs/PERFORMANCE.md § 5.
#   · it emits TWO packages from ONE build, no recompilation:
#       - a `.tar.gz` laid out like `cmake --install` (bin/POM2 +
#         share/POM2/{roms,fonts,pic}) for a Pi OS **Lite** cabinet, which has
#         no FUSE and would have to extract an AppImage on every boot;
#       - an `.AppImage` for Pi OS **with a desktop**, where one clickable file
#         is what you want.
#
#  Why bookworm: Raspberry Pi OS IS Debian bookworm (glibc 2.36). Building on
#  the runner's own ubuntu-24.04-arm userland stamps GLIBC_2.39 and the binary
#  starts on no Pi at all. The floor is checked below and the build fails if it
#  is exceeded.
#
#  Honoured env: POM2_MCPU (default cortex-a72), POM2_VERSION, POM2_PGO=0 to
#  fall back to a single pass (script debugging / a quick build).
#
#  (c) 2026 VERHILLE Arnaud — POM2.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/../.."

MCPU="${POM2_MCPU:-cortex-a72}"   # cortex-a72 = Pi 4 / Pi 400, a76 = Pi 5, a53 = Pi 3
DO_PGO="${POM2_PGO:-1}"

case "$MCPU" in
    cortex-a72) PKG_TAG=pi400 ;;   # Pi 4 / Pi 400
    cortex-a76) PKG_TAG=pi5   ;;
    cortex-a53) PKG_TAG=pi3   ;;
    *)          PKG_TAG="$MCPU" ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    ca-certificates git wget curl file desktop-file-utils \
    build-essential cmake pkg-config binutils \
    zlib1g-dev \
    libglfw3-dev \
    libgles2-mesa-dev libegl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
    libslirp-dev

# The bind-mounted repo is owned by the host uid, not root.
git config --global --add safe.directory '*'

echo "[pi-build] target: -mcpu=$MCPU  (package tag: $PKG_TAG)"
echo 'int main(){}' | g++ -x c++ -mcpu="$MCPU" -o /dev/null - \
    || { echo "ERROR: -mcpu=$MCPU rejected by $(g++ --version | head -1)"; exit 1; }

# --- AppImage tooling -------------------------------------------------------
# Shared with packaging/linux/build_in_bookworm_arm64.sh — pinned digests and
# the appimagetool/runtime pairing rationale live in the fetch script.
#
# This block used to be a fourth hand-kept copy of that list, and it cost a
# release cycle: when the other three moved to AppImageKit-12's appimagetool
# (the only one whose compressor the pinned ET_EXEC runtime can read), this one
# stayed on 1.9.1 and produced a zstd image that could not open itself.
TOOLS=/opt/appimage-tools
ARCH=aarch64 /work/packaging/linux/fetch_appimage_tools.sh "$TOOLS"
export POM2_APPIMAGE_TOOLS_DIR="$TOOLS"

# --- Dear ImGui (pinned COMMIT from imgui_pin.env) --------------------------
tools/fetch_imgui_pinned.sh

# --- Build ------------------------------------------------------------------
# ⚠ CMAKE_CXX_FLAGS and NOT CMAKE_CXX_FLAGS_RELEASE — the latter carries the
# -O3 the build type sets, and overriding it on the command line would drop it.
# -static-libstdc++/-static-libgcc so the only libc-family floor is bookworm's
# glibc 2.36; bookworm's g++ would otherwise raise it through GLIBCXX symbols.
ARCH_FLAGS="-mcpu=$MCPU -mtune=$MCPU"
BUILD_DIR=build-pi400
PROFDIR="$PWD/build-pi400-profile"

configure() {                # configure <extra-flags> <IPO ON/OFF>
    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DPOM2_ENABLE_TESTS=OFF \
        -DPOM2_GLES=ON \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION="$2" \
        -DCMAKE_C_FLAGS="$ARCH_FLAGS $1" \
        -DCMAKE_CXX_FLAGS="$ARCH_FLAGS $1" \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc $1"
}

# The TUs that carry POM2's hot loop (docs/PERFORMANCE.md § 2). If any of them
# comes out of training without counters, the training run executed nothing
# useful and the PGO would be a placebo — fail rather than ship that.
HOT_TUS="M6502 Memory DiskIICard DiskImage Apple2Display"

if [ "$DO_PGO" = "1" ]; then
    # ⚠ BOTH PASSES SHARE ONE BUILD DIRECTORY. GCC names each .gcda after the
    # ABSOLUTE PATH of the object it belongs to: instrument in build-A, read
    # back from build-B, and NO profile is found — while -Wno-missing-profile
    # (needed anyway for the ImGui/GLFW TUs that are never trained) makes that
    # failure perfectly silent. The binary comes out with zero gain and zero
    # diagnostics. Hence the explicit checks below.
    rm -rf "$PROFDIR"; mkdir -p "$PROFDIR"

    echo "[pi-build] PGO pass 1/2 — instrumented pom2_bench"
    configure "-fprofile-generate=$PROFDIR" OFF
    cmake --build "$BUILD_DIR" --parallel "${POM2_JOBS:-2}" --target pom2_bench

    echo "[pi-build] PGO — training run"
    packaging/raspberry/pgo_train.sh "$BUILD_DIR/pom2_bench" "$PWD"

    echo "[pi-build] profiles collected: $(find "$PROFDIR" -name '*.gcda' | wc -l)"
    for must in $HOT_TUS; do
        find "$PROFDIR" -name "*${must}*.gcda" | grep -q . \
            || { echo "ERROR: no profile for $must — the training run was mute"; exit 1; }
    done

    # ⚠ SECOND HALF OF THE SAME TRAP, and it is POM2-specific. The training
    # driver is `pom2_bench`; the shipped binary is `POM2` (target
    # pom2_imgui). CMake compiles each target's sources into ITS OWN object
    # directory, so the emulator core exists twice on disk —
    #     CMakeFiles/pom2_bench.dir/src/Memory.cpp.o
    #     CMakeFiles/pom2_imgui.dir/src/Memory.cpp.o
    # — and the .gcda is named after the first only. Without this copy, pass 2
    # rebuilds POM2 with no profile at all for exactly the files that matter,
    # again silently. GCC mangles the object path into the file name by turning
    # '/' into '#', so retargeting is a pure string substitution.
    # (NeoST does not need this: its GUI and headless share one `neost_core`
    # library, hence one set of objects.)
    copied=0
    for f in "$PROFDIR"/*pom2_bench.dir*.gcda; do
        [ -e "$f" ] || continue
        cp -f "$f" "${f//pom2_bench.dir/pom2_imgui.dir}" && copied=$((copied+1))
    done
    echo "[pi-build] $copied profiles mapped onto the pom2_imgui objects"
    for must in $HOT_TUS; do
        find "$PROFDIR" -name "*pom2_imgui.dir*${must}*.gcda" | grep -q . \
            || { echo "ERROR: $must has no profile under the pom2_imgui objects —"; \
                 echo "       the shipped binary would get NO benefit."; exit 1; }
    done

    # ── Reference signature, taken from the PASS-1 binary ────────────────
    # The whole claim behind this build is "PGO + LTO change the code layout,
    # never the semantics". That is a claim you can TEST, on this machine, for
    # free: run a fixed workload now with the instrumented (un-optimised,
    # un-profiled) binary, run the same one after pass 2, and compare.
    #
    # It has to be done here because pass 2 overwrites the binary. Only the
    # hash and cycle fields are compared — wall/speed obviously differ.
    #
    # ⚠ LC_ALL=C on the glob. Without it the "first .dsk" depends on the
    # host's collation (a French desktop picks buzzard_bait, a C-locale runner
    # picks CRIME_A), so the two sides would silently measure different disks
    # — which is exactly how the first version of this check compared two
    # unrelated runs and looked fine.
    REF_DISK="$(LC_ALL=C ls disks_5.4/dsk/*.dsk 2>/dev/null | head -1 || true)"
    sig() {   # sig <binary> <args…>  → "cycles=… ram=… fb=…"
        local b="$1"; shift
        "$b" "$@" --quiet 2>/dev/null | tail -1 \
            | grep -oE 'cycles=[0-9]+|ram=[0-9a-f]+|fb=[0-9a-f]+' | tr '\n' ' '
    }
    REF_CPU="$(sig "$BUILD_DIR/pom2_bench" --frames 3000)"
    REF_DSK=""
    [ -n "$REF_DISK" ] && REF_DSK="$(sig "$BUILD_DIR/pom2_bench" --disk "$REF_DISK" --frames 900)"
    echo "[pi-build] reference (pass-1 binary): $REF_CPU"
    [ -n "$REF_DSK" ] && echo "[pi-build] reference (disk, $(basename "$REF_DISK")): $REF_DSK"

    echo "[pi-build] PGO pass 2/2 — final build (profile + LTO)"
    # -fprofile-partial-training: the untrained objects (the GUI's main.cpp,
    # ImGui, miniaudio) get optimised normally instead of being treated as cold
    # — without it the windowed frontend would come out degraded.
    # -fprofile-correction: counters from a multi-threaded program can be
    # slightly inconsistent; repair rather than fail.
    configure "-fprofile-use=$PROFDIR -fprofile-correction -fprofile-partial-training -Wno-missing-profile" ON
    cmake --build "$BUILD_DIR" --parallel "${POM2_JOBS:-2}" 2>&1 | tee /tmp/pom2-pgo-build.log

    # Belt and braces: the warning is suppressed for every TU, so ask the log
    # about the core sources explicitly. If the profile paths ever drift again,
    # this is what says so instead of a quietly slower binary.
    if grep -E "src/(M6502|Memory|DiskIICard|DiskImage|Apple2Display)\.cpp.*(profile count data file not found|missing-profile)" \
            /tmp/pom2-pgo-build.log >/dev/null 2>&1; then
        echo "ERROR: a core source was compiled with no profile (build paths out of step?)"
        exit 1
    fi

    # ── The output-identity gate ─────────────────────────────────────────
    # Same machine, same workloads, pass-1 binary vs the final PGO+LTO one.
    # If these differ, the fast binary is not the same emulator and the whole
    # recipe is void — that is a build failure, not a footnote.
    NEW_CPU="$(sig "$BUILD_DIR/pom2_bench" --frames 3000)"
    if [ "$NEW_CPU" != "$REF_CPU" ]; then
        echo "ERROR: PGO+LTO changed the emulator's output (CPU workload)"
        echo "  pass 1: $REF_CPU"
        echo "  pass 2: $NEW_CPU"
        exit 1
    fi
    if [ -n "$REF_DSK" ]; then
        NEW_DSK="$(sig "$BUILD_DIR/pom2_bench" --disk "$REF_DISK" --frames 900)"
        if [ "$NEW_DSK" != "$REF_DSK" ]; then
            echo "ERROR: PGO+LTO changed the emulator's output (disk workload)"
            echo "  pass 1: $REF_DSK"
            echo "  pass 2: $NEW_DSK"
            exit 1
        fi
    fi
    echo "[pi-build] OK: output byte-identical to the un-profiled build"
else
    echo "[pi-build] PGO disabled (POM2_PGO=0) — single pass"
    configure "" ON
    cmake --build "$BUILD_DIR" --parallel "${POM2_JOBS:-2}"
fi

test -x "$BUILD_DIR/POM2" || { echo "ERROR: GUI frontend not built (GLFW?)"; exit 1; }

# --- Checks that must fail HERE, not on the Pi ------------------------------
readelf -h "$BUILD_DIR/POM2" | grep -q AArch64 \
    || { echo "ERROR: not an AArch64 binary"; exit 1; }
MAX=$(objdump -T "$BUILD_DIR/POM2" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -V | tail -1)
echo "[pi-build] highest glibc symbol required: ${MAX:-none}"
test "$(printf '%s\nGLIBC_2.36\n' "$MAX" | sort -V | tail -1)" = "GLIBC_2.36" \
    || { echo "ERROR: requires $MAX > GLIBC_2.36 — will not run on Pi OS"; exit 1; }
# THE point of the GLES tier: on a Pi, desktop libGL is the software
# rasteriser, so linking it is a silent ~2 fps regression rather than an error.
if ldd "$BUILD_DIR/POM2" | grep -qE 'lib(GL|OpenGL)\.so'; then
    echo "ERROR: POM2 links desktop libGL"; ldd "$BUILD_DIR/POM2" | grep -E 'GL|EGL'; exit 1
fi
ldd "$BUILD_DIR/POM2" | grep -qE 'GLES' \
    || { echo "ERROR: POM2 does not link GLESv2 — is POM2_GLES really on?"; exit 1; }

# --- Package 1: tar.gz for a Pi OS Lite cabinet -----------------------------
# Exactly the `cmake --install` layout, which is what ResourcePaths resolves
# (<exeDir>/../share/POM2) and what build_native_pi.sh --install produces — so
# a cabinet set up from either route has the same tree.
VERSION="${POM2_VERSION:-$(sed -n 's/^project(pom2_imgui VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
[ -n "$VERSION" ] || VERSION="0.0"
STAGE="dist/$PKG_TAG"
rm -rf "$STAGE"; mkdir -p dist
cmake --install "$BUILD_DIR" --prefix "$STAGE"
# The bench is not in the install rules, but it is what you re-measure with
# after a change — and the README tells the user to run it. Ship it beside the
# binary it measures.
install -m 755 "$BUILD_DIR/pom2_bench" "$STAGE/bin/"
TGZ="dist/POM2-v${VERSION}-${PKG_TAG}-aarch64.tar.gz"
tar -czf "$TGZ" -C "$STAGE" .
echo "[pi-build] OK: $TGZ"

# --- Package 2: AppImage from the SAME build --------------------------------
# ⚠ It does NOT replace the release AppImage (`POM2-v<ver>-aarch64.AppImage`,
# generic aarch64, Pi 3 → Pi 5): this one is compiled for ONE core. The name
# must differ — a release's publish job flattens every artifact into one
# directory, and two same-named packages would overwrite each other in silence.
# Tagged through POM2_APPIMAGE_VARIANT, which build_appimage.sh inserts before
# the arch. This used to be done by folding the tag into POM2_VERSION
# (`0.8-pi400`), which produced the right FILE name but also stamped a bogus
# version into the artifact — and the release workflow asserts that the binary
# announces the tag's version, so the honest seam is the one that only touches
# the name.
export POM2_APPIMAGE_SKIP_BUILD=1
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=aarch64
export POM2_APPIMAGE_RUNTIME="$TOOLS/runtime-aarch64"
POM2_APPIMAGE_VARIANT="$PKG_TAG" packaging/linux/build_appimage.sh "$BUILD_DIR" dist

ls -lh dist/*.AppImage "$TGZ"
