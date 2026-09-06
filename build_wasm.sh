#!/usr/bin/env bash
# POM2 — WebAssembly build driver.
#
# Output: wasm/
#   index.html        — entry point (renamed from POM2.html)
#   POM2.js           — Emscripten loader
#   POM2.wasm         — compiled module
#   POM2.data         — preloaded asset bundle. Its CONTENTS are decided by
#                       packaging/bundle.manifest, parsed by CMakeLists.txt
#                       (`dir` + `file` + `wasm` entries), not by this script:
#                       today roms/ + fonts/ + the two pic/ photos + floppyemu/
#   POM2.worker.js    — pthread worker (when USE_PTHREADS=1)
#   serve.py          — local dev server that sets COOP+COEP headers
#                       (required for SharedArrayBuffer / pthreads)
#
# Prerequisites:
#   - Emscripten SDK on PATH (`emcc -v` must succeed).
#       Install:  https://emscripten.org/docs/getting_started/downloads.html
#       Then:     source path/to/emsdk/emsdk_env.sh
#   - The host `./setup_imgui.sh` must have populated `imgui/` already.
#
# Usage:
#   ./build_wasm.sh                # release build
#   ./build_wasm.sh --clean        # nuke build_wasm/ first
#   ./build_wasm.sh --serve        # build + launch dev server on :8080
#   ./build_wasm.sh --with-data    # also bundle the disks_3.5/ library into
#                                 # POM2.data. LOCAL DEV ONLY: disks_3.5 is
#                                 # `deny`-listed in packaging/bundle.manifest,
#                                 # so CMake warns and the resulting bundle
#                                 # must not be published.
#   POM2_WASM_ALLOW_LARGE_DATA=1 ./build_wasm.sh --with-data
#                                 # allow a local POM2.data over GitHub's 100 MiB limit

set -euo pipefail

cd "$(dirname "$0")"

CLEAN=0
SERVE=0
WITH_DATA=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --serve) SERVE=1 ;;
        --with-data) WITH_DATA=1 ;;
        -h|--help)
            sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v emcc >/dev/null 2>&1; then
    cat >&2 <<EOF
emcc not found. Install + activate the Emscripten SDK:
  git clone https://github.com/emscripten-core/emsdk.git
  cd emsdk && ./emsdk install latest && ./emsdk activate latest
  source ./emsdk_env.sh
EOF
    exit 1
fi

if [ ! -d imgui ]; then
    echo "imgui/ not found — running ./setup_imgui.sh first..."
    ./setup_imgui.sh
fi

BUILD_DIR=build_wasm
WASM_DIR=wasm
GITHUB_FILE_LIMIT_BYTES=$((100 * 1024 * 1024))

if [ $CLEAN -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR" "$WASM_DIR"

# Configure with the Emscripten toolchain.
CMAKE_EXTRA=()
# NO -DPOM2_WASM_BUNDLE here. It used to pass "fonts;pic;floppyemu" and NOTHING
# HAS EVER READ IT — the payload moved into packaging/bundle.manifest, and this
# variable survived as a list that looked authoritative (it is already wrong:
# it omits roms/ and names the whole pic/ folder) while deciding nothing.
if [ $WITH_DATA -eq 1 ]; then
    CMAKE_EXTRA+=(-DPOM2_WASM_BUNDLE_DISKS=ON)
else
    CMAKE_EXTRA+=(-DPOM2_WASM_BUNDLE_DISKS=OFF)
fi
emcmake cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPOM2_ENABLE_TESTS=OFF \
    "${CMAKE_EXTRA[@]}"

# Keep large C++ translation units from exhausting RAM. Override explicitly on
# a builder known to have more headroom.
cmake --build "$BUILD_DIR" --parallel "${POM2_JOBS:-2}"

# Refuse a --with-data bundle above GitHub's file limit: wasm/ is no longer
# tracked (CI deploys it to Pages), but a bundle that size is a mistake
# everywhere it could land — the release zip, the Pages artifact, a commit.
DATA_SRC="$BUILD_DIR/POM2.data"
if [ -f "$DATA_SRC" ]; then
    DATA_SIZE_BYTES=$(wc -c < "$DATA_SRC" | tr -d '[:space:]')
    if [ "$DATA_SIZE_BYTES" -gt "$GITHUB_FILE_LIMIT_BYTES" ] && [ "${POM2_WASM_ALLOW_LARGE_DATA:-0}" != "1" ]; then
        DATA_SIZE_MIB=$(( (DATA_SIZE_BYTES + 1024 * 1024 - 1) / (1024 * 1024) ))
        cat >&2 <<EOF
error: $DATA_SRC is ${DATA_SIZE_MIB} MiB, above GitHub's 100 MiB file limit.

Refusing to copy it into $WASM_DIR/.
For a local-only large bundle, rerun with:
  POM2_WASM_ALLOW_LARGE_DATA=1 ./build_wasm.sh --with-data
EOF
        exit 1
    fi
fi

# Stage artefacts into wasm/.
ARTEFACTS=(POM2.html POM2.js POM2.wasm POM2.data POM2.worker.js)
for f in "${ARTEFACTS[@]}"; do
    src="$BUILD_DIR/$f"
    if [ -f "$src" ]; then
        cp -f "$src" "$WASM_DIR/"
    fi
done
# Rename the entry document so the deployment URL is clean.
if [ -f "$WASM_DIR/POM2.html" ]; then
    mv -f "$WASM_DIR/POM2.html" "$WASM_DIR/index.html"
fi

# Local dev server with COOP + COEP — required for SharedArrayBuffer /
# pthreads. python3's built-in http.server doesn't ship those headers,
# so we provide a tiny wrapper.
cat > "$WASM_DIR/serve.py" <<'PY'
#!/usr/bin/env python3
"""Local dev server for POM2 WASM with COOP+COEP headers.

Usage:  python3 serve.py [port]   # defaults to 8080
"""
import http.server, sys, os
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
os.chdir(os.path.dirname(os.path.abspath(__file__)))
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy",   "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()
print(f"POM2 WASM dev server: http://127.0.0.1:{PORT}/")
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
PY
chmod +x "$WASM_DIR/serve.py"

echo
echo "── done ────────────────────────────────────────────"
echo "  artefacts: $WASM_DIR/"
ls -lh "$WASM_DIR/" | awk 'NR>1 {printf "    %s %s\n", $5, $9}'
echo "  serve:    cd $WASM_DIR && python3 serve.py"
echo "  open:     http://127.0.0.1:8080/"
echo

if [ $SERVE -eq 1 ]; then
    exec python3 "$WASM_DIR/serve.py"
fi
