#!/usr/bin/env bash
# check_ctest_skips.sh <build-dir> — after `ctest`, fail if a test SKIPPED.
#
# CI fetches every ROM and test disk (tools/fetch_test_media.py), so a test that
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
  $'videx_videoterm_boot\t*' \
  $'card_snapshot_contract\tVideoterm+ROM' )
# Since 2026-11-01 the test-media archive (pom2_media_202611) carries every disk and
# ROM the suite opens — the SoftCard II's CP/M 2.28B, the Applicard's boot
# disk and ROM, the UltraTerm's ROMs, the Premium SoftCard's CP/M 2.25 and
# 2.26, the a2audit and mb-audit disks, Saturn's software disk — so
# softcard2_card, applicard_card, videx_ultraterm, premium_softcard, a2audit,
# mb_audit and saturn_card may not skip any more. A new medium goes into the
# archive AND tools/test_media.sha256, not into this list.
# videx_videoterm_boot is the one ROM-gated test whose dumps are deliberately
# NOT in the tree: the Videoterm firmware and character ROMs are third-party
# and are fetched at run time (ROM Status → RetroBIOS a2vidtrm.zip), pending
# the legal-payload gate (TODO G1). The card itself stays covered on CI by
# videx_videoterm_bus / _render / _snapshot, which use synthetic ROMs.
# card_snapshot_contract's Videoterm+ROM entry skips for the same reason; its
# bare VidexVideoterm entry still runs every contract phase.
# The two CRT tests depend on an OpenGL context (the main Linux leg has none;
# the `gl-software` job gives them one and requires that they run). The main
# Linux job installs libslirp, so slirp_loopback_fence may not skip.
# Since 2026-11-02 it also installs ghostscript and the fr_FR.UTF-8 locale,
# so postscript_render's bounded-raster jobs and imagewriter_pdf's
# decimal-comma case may not skip either.

scan() {  # scan <LastTest.log> → "test<TAB>line" for every printed skip
    # ANYWHERE in the line and case-insensitively, not `^SKIP`. Two
    # independent audits (2026-11-01) found ~33 sites in 24 files that skip a
    # SUB-CASE, print a word this detector could not see and exit 0 — so the
    # one guard built to catch "a missing fixture must not read as a pass" was
    # blind to all of them. The spellings in the tree: `[SKIP]` (first char is
    # `[`), lowercase `skip:`, `WARN`, `NOTE: … skipped`, `(skipped: …)` and
    # `[ OK ] … (SKIP the comparison…)`. The worst case was
    # `diskii_lss_smoke`, where 7 of 11 cases — including the two the project
    # pins its Disk II data-separator fixes with — `return true` on a missing
    # `roms/diskii_p6.rom` under a green "diskii_lss_smoke OK".
    #
    # `ctest` is excluded because its own progress lines carry the word
    # ("***Skipped" is scanConsole's job, and `N/M Test #K: name ... Skipped`
    # would double-report).
    # ANCHORED on purpose: a bare substring match also hits ctest's own
    # framing lines for a test whose NAME contains the word, and legitimate
    # output from a test ABOUT skipping (display_dirty_skip measures frame
    # skipping). So match the announcement shapes instead, case-insensitively.
    awk '
        /^[0-9]+\/[0-9]+ Testing: / { t = $3; next }
        {
            line = $0
            sub(/^[ \t]+/, "", line)
            low = tolower(line)
            # Deliberately NOT a bare /\(skip/: a test is allowed to talk
            # about skipping (display_dirty_skip measures frame skipping and
            # signs off "OK (skip is bit-identical to full repaint)"). A test
            # announcing that it skipped SOMETHING says so at the start of the
            # line, or with the word "skipped".
            if (low ~ /^\[?[ \t]*skipp?(ed|ing)?[^a-z]/ ||
                low ~ /\(skipped/ ||
                low ~ /^(warn|note)[^a-z].*skipp?(ed)?[^a-z]/)
                print t "\t" line
        }
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
    printf '1/1 Testing: slirp_loopback_fence\nOutput:\nSKIP: built without libslirp\nTest Passed.\n' > "$L"
    printf '1/1 Test #7: slirp_loopback_fence ...***Skipped   0.00 sec\n' > "$tmp/console"
    if bash "$here" "$tmp/b" "$tmp/console" >/dev/null; then
        echo "self-test FAILED: a libslirp skip passed"; exit 1
    fi
    echo '  ok: the libslirp test must run'
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
