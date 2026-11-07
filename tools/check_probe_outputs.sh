#!/usr/bin/env bash
# check_probe_outputs.sh — a probe writes under pom2test::probeOutDir()
# (tests/ProbeOutDir.h: --out, then $POM2_PROBE_OUT, then $TMPDIR/pom2_probes),
# never into the cwd or a fixed /tmp path. Two probes did (bug hunt
# 2026-11-01 round 3, CLI-3 / CLI-4): one wrote <base>.hgr1/2 into the repo
# root it requires as its cwd, one hard-coded /tmp. This scan fails on an
# fopen / ofstream whose path is a string literal, or on a "/tmp/" literal, in
# any probe source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bad=0
for f in "$root"/tests/*probe*.cpp "$root"/tests/*_dump.cpp; do
    [ -f "$f" ] || continue
    while IFS= read -r hit; do
        echo "${f#$root/}:$hit" >&2; bad=1
    done < <(grep -n -E '(fopen|ofstream[^;]*)\(\s*"|"/tmp/' "$f" | grep -v -E '^\s*[0-9]+:\s*//' || true)
done
[ "$bad" -eq 0 ] || { echo "check_probe_outputs: write under pom2test::probeOutDir() (tests/ProbeOutDir.h)" >&2; exit 1; }
echo "check_probe_outputs: no probe writes to a literal path"
