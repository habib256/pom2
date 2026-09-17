#!/usr/bin/env bash
# check_ctest_skips.sh <build-dir> — after `ctest`, fail if a test SKIPPED.
#
# The repository tracks its ROMs and its test media, so on CI a test that
# skips has lost something it needs — a dump renamed, a probe path moved — and
# the green suite hides that the L0 path stopped being exercised (TODO G5-15).
# Two shapes are caught:
#   · a line of test output that starts with "SKIP", from
#     <build>/Testing/Temporary/LastTest.log — tests that print it and return
#     0, which ctest counts as a pass (and a SKIP_RETURN_CODE test, which
#     that log ALSO records as "Test Passed.");
#   · ctest's own "***Skipped" status, from its console output, which is the
#     only place it appears — pass that file as the second argument
#     (`ctest ... | tee ctest-console.log`).
# ALLOW lists the deliberate ones, as `test-name <tab> output fragment`.
#
# Usage: tools/check_ctest_skips.sh <build-dir> [<ctest-console-log>] | --self-test
set -euo pipefail
export LC_ALL=C

ALLOW=$(printf '%s\n' \
  $'iic_onboard_smartport_smoke\tPOM2_TRACE_HDV' \
  $'crt_glass_resample\t*' \
  $'crt_barrel_view\t*' \
  $'slirp_loopback_fence\t*' )
# The last three depend on the HOST, not on the tree: the two CRT tests need
# an OpenGL context (the main Linux leg has none; the `gl-software` job gives
# them one and requires that they RUN) and
# slirp_loopback_fence needs libslirp, which the CI image does not install.

scan() {  # scan <LastTest.log> → "test<TAB>line" for every printed skip
    awk '
        /^[0-9]+\/[0-9]+ Testing: / { t = $3; next }
        /^[ \t]*SKIP/               { sub(/^[ \t]+/, ""); print t "\t" $0 }
    ' "$1"
}
scanConsole() {  # scan <ctest console log> → "test<TAB>(ctest: Skipped)"
    awk '/\*\*\*Skipped/ { for (i = 1; i <= NF; ++i) if ($i ~ /^#[0-9]+:$/) { print $(i+1) "\t(ctest status: Skipped)"; break } }' "$1"
}

self_test() {
    local tmp here
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' RETURN
    here="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    mkdir -p "$tmp/b/Testing/Temporary"
    L="$tmp/b/Testing/Temporary/LastTest.log"
    printf '1/2 Testing: alpha\nOutput:\nall good\nTest time =   0.1 sec\nTest Passed.\n' > "$L"
    bash "$here" "$tmp/b" >/dev/null || { echo "self-test FAILED: a clean log was refused"; exit 1; }
    echo '  ok: a clean log passes'
    printf '2/2 Testing: beta\nOutput:\n  SKIP: roms/x.rom not found\nTest Passed.\n' >> "$L"
    if bash "$here" "$tmp/b" >/dev/null; then echo "self-test FAILED: a printed SKIP passed"; exit 1; fi
    echo '  ok: a test that prints SKIP and passes is caught'
    printf '1/1 Testing: gamma\nOutput:\nall good\nTest Passed.\n' > "$L"
    printf '1/1 Test #7: gamma ..........***Skipped   0.01 sec\n' > "$tmp/console"
    if bash "$here" "$tmp/b" "$tmp/console" >/dev/null; then echo "self-test FAILED: a status skip passed"; exit 1; fi
    echo '  ok: a ctest ***Skipped status in the console log is caught'
    printf '1/1 Testing: iic_onboard_smartport_smoke\nOutput:\n  SKIP: POM2_TRACE_HDV non pose\nTest Passed.\n' > "$L"
    bash "$here" "$tmp/b" >/dev/null || { echo "self-test FAILED: an allowlisted skip was refused"; exit 1; }
    echo '  ok: an allowlisted skip passes'
    printf '1/1 Testing: crt_barrel_view\nOutput:\nSKIP: glfwInit failed (no display)\nTest Passed.\n' > "$L"
    printf '1/1 Test #198: crt_barrel_view ...***Skipped   0.00 sec\n' > "$tmp/console"
    bash "$here" "$tmp/b" "$tmp/console" >/dev/null || { echo "self-test FAILED: a host-dependent skip was refused in one of its two shapes"; exit 1; }
    echo '  ok: a host-dependent skip passes in both its shapes'
    rm "$L"
    if bash "$here" "$tmp/b" >/dev/null 2>&1; then echo "self-test FAILED: a missing log passed"; exit 1; fi
    echo '  ok: no log is a failure'
    echo "check_ctest_skips: self-test passed"
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit 0; fi

BUILD="${1:?usage: check_ctest_skips.sh <build-dir> [<ctest-console-log>] | --self-test}"
LOG="$BUILD/Testing/Temporary/LastTest.log"
[ -f "$LOG" ] || { echo "check_ctest_skips: $LOG not found — run ctest first" >&2; exit 2; }

bad=0
while IFS=$'\t' read -r test line; do
    [ -n "$test" ] || continue
    allowed=0
    while IFS=$'\t' read -r at frag; do
        [ -n "$at" ] || continue
        # A fragment of `*` allows the test whatever it printed: a host-
        # dependent skip shows up twice (its own SKIP line in LastTest.log,
        # ctest's status in the console), with different words.
        if [ "$at" = "$test" ] && { [ "$frag" = "*" ] || [[ "$line" == *"$frag"* ]]; }; then
            allowed=1
        fi
    done <<< "$ALLOW"
    [ "$allowed" = 1 ] && continue
    echo "SKIPPED  $test: $line"
    bad=1
done < <(scan "$LOG"; if [ -n "${2:-}" ]; then scanConsole "$2"; fi)

if [ "$bad" != 0 ]; then
    echo "check_ctest_skips: FAILED — a test skipped on a tree that carries its ROMs and media"
    exit 1
fi
echo "check_ctest_skips: no unexpected skips"
