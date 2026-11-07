#!/usr/bin/env bash
# check_glfw_abort_stubs.sh — every GLFW entry point the pinned Emscripten
# implements as abort() must be in key_chord_policy's kAbortStubs list.
#
# The test scans the sources for those names outside an __EMSCRIPTEN__ guard;
# a call to one tears the whole WASM module down. The list is static (the
# Linux ctest job has no emsdk), derived from emscripten 6.0.8; this script
# re-derives it from the toolchain $EMSDK points at — the CI WASM job's
# pinned one — so the list cannot drift silently when the pin moves (bug hunt
# 2026-11-01 round 3, WASM-6).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
js="${EMSDK:?EMSDK is not set}/upstream/emscripten/src/lib/libglfw.js"
[ -f "$js" ] || { echo "check_glfw_abort_stubs: $js not found" >&2; exit 1; }
test_src="$root/tests/key_chord_policy_test.cpp"
derived=$(grep -o -E '^[[:space:]]+glfw[A-Za-z0-9_]+: \([^)]*\) => abort\(' "$js" |
          sed -E 's/^[[:space:]]+(glfw[A-Za-z0-9_]+):.*/\1/' | sort -u)
[ -n "$derived" ] || { echo "check_glfw_abort_stubs: no abort stub found in $js" >&2; exit 1; }
missing=0
for name in $derived; do
    grep -q "\"$name\"" "$test_src" || { echo "missing from kAbortStubs: $name" >&2; missing=1; }
done
[ "$missing" -eq 0 ] || { echo "check_glfw_abort_stubs: update kAbortStubs in $test_src" >&2; exit 1; }
echo "check_glfw_abort_stubs: $(echo "$derived" | wc -l | tr -d ' ') abort stubs, all listed"
