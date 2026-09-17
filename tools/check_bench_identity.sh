#!/usr/bin/env bash
# check_bench_identity.sh <pom2_bench> [--update]
#
# Runs every workload in tests/bench_golden.txt and compares the cycle count
# and RAM hash with the recorded ones (TODO G5-11). docs/PERFORMANCE.md makes
# "the bench hashes do not move" the condition for every hot-path change; the
# only automated check of it lived in the Raspberry Pi release job.
#
# --update rewrites the golden values from this binary. Use it only for a
# change that is meant to alter emulation, and say so in the commit.
set -euo pipefail
BENCH="${1:?usage: check_bench_identity.sh <pom2_bench> [--update]}"
MODE="${2:-check}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOLDEN="$ROOT/tests/bench_golden.txt"
# Absolute before the cd below: the workloads' paths are repo-relative.
case "$BENCH" in /*) ;; *) BENCH="$PWD/$BENCH" ;; esac
cd "$ROOT"
[ -x "$BENCH" ] || { echo "check_bench_identity: $BENCH is not executable" >&2; exit 2; }

bad=0; n=0
tmp="$(mktemp)"; trap 'rm -f "$tmp"' EXIT
while IFS= read -r line; do
    case "$line" in ''|'#'*) printf '%s\n' "$line" >> "$tmp"; continue ;; esac
    args=$(printf '%s' "$line" | awk -F'|' '{gsub(/^ +| +$/,"",$1); print $1}')
    wantCycles=$(printf '%s' "$line" | awk -F'|' '{gsub(/ /,"",$2); print $2}')
    wantRam=$(printf '%s' "$line" | awk -F'|' '{gsub(/ /,"",$3); print $3}')
    # shellcheck disable=SC2086 — the args are a word list on purpose
    out=$("$BENCH" $args --quiet 2>/dev/null | grep '^frames=' | tail -1 || true)
    gotCycles=$(printf '%s' "$out" | sed -n 's/.*cycles=\([0-9]*\).*/\1/p')
    gotRam=$(printf '%s' "$out" | sed -n 's/.* ram=\([0-9a-f]*\).*/\1/p')
    n=$((n + 1))
    if [ -z "$gotRam" ]; then
        echo "NO RESULT  $args  (bench output: ${out:-nothing})"
        bad=1
        printf '%s\n' "$line" >> "$tmp"
        continue
    fi
    printf '%s | %s | %s\n' "$args" "$gotCycles" "$gotRam" >> "$tmp"
    if [ "$gotCycles" != "$wantCycles" ] || [ "$gotRam" != "$wantRam" ]; then
        echo "MOVED      $args"
        echo "           cycles $wantCycles -> $gotCycles, ram $wantRam -> $gotRam"
        [ "$MODE" = "--update" ] || bad=1
    else
        echo "same       $args"
    fi
done < "$GOLDEN"

[ "$n" -gt 0 ] || { echo "check_bench_identity: no workloads in $GOLDEN" >&2; exit 2; }
if [ "$MODE" = "--update" ]; then
    [ "$bad" = 0 ] || { echo "check_bench_identity: a workload produced no result; golden not rewritten" >&2; exit 1; }
    cp "$tmp" "$GOLDEN"
    echo "check_bench_identity: golden rewritten ($n workloads)"
    exit 0
fi
if [ "$bad" != 0 ]; then
    echo "check_bench_identity: FAILED — the emulation moved (docs/PERFORMANCE.md § rule)"
    exit 1
fi
echo "check_bench_identity: $n workloads identical"
