#!/usr/bin/env bash
# check_settings_keys.sh — every settings key POM2 reads is written by
# something, and every key it writes is read by something (TODO G5-7).
#
# `settings_roundtrip` pins the storage engine; nothing pinned the KEYS. A
# key read with a default and written by nobody is a preference the user
# can set only by editing state.cfg; a key written and read by nobody is
# dead weight that looks like persistence. Both shapes were in the tree
# (`applewin_submode` was written on every quit and ignored on load).
#
# Scans `get|set{String,Bool,Int,Float,Double}("literal"` under src/. A key
# built at run time is seen by its literal prefix only, so the prefixes
# below are declared on both sides. The slot-card keys (`slot_<N>_card`,
# `iic_expansion_card`) come from `pom2::slotCardSettingKey` and are read
# and written only through it, so no literal of theirs is left to scan.
# Anything else that is deliberately
# one-sided goes in ALLOW with its reason.
#
# Usage: tools/check_settings_keys.sh [--self-test]
# POM2_SETTINGS_ROOT overrides the tree to scan (the self-test uses it).
set -euo pipefail
export LC_ALL=C

# key  reason — one per line. Keep the reason: it is the only record of why.
ALLOW=$(cat <<'LIST'
clock_card_enable          legacy key, read once to migrate to slot_N_card
slot_4_card                legacy key, read by the clock-card migration
disk_writeback_slot        prefix: disk_writeback_slot<N>, read by the notch migration
ethernet_backend           hand-edited: slirp | loopback | none (DEV § Network backends)
uthernet_allow_loopback    hand-edited opt-in to reach the host loopback (CLAUDE.md)
uthernet_slirp_restricted  hand-edited opt-in, slirp virtual services only
transwarp_dsw1             hand-edited DIP switch bank (TransWarp)
transwarp_dsw2             hand-edited DIP switch bank (TransWarp)
fujinet_builtin_network    prefix: fujinet_builtin_network_slot<N>, hand-edited
LIST
)

self_test() {
    local tmp here
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    here="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    mkdir -p "$tmp/src"
    run() { POM2_SETTINGS_ROOT="$tmp" bash "$here" >"$tmp/out" 2>&1; }
    printf 's.setBool("both_ways", true);\nbool b = s.getBool("both_ways", false);\n' > "$tmp/src/a.cpp"
    run || { cat "$tmp/out"; echo "self-test FAILED: a symmetric key was refused"; exit 1; }
    echo '  ok: a key read and written passes'
    printf 'int x = s.getInt("only_read", 3);\n' > "$tmp/src/b.cpp"
    if run; then echo "self-test FAILED: a read-only key passed"; exit 1; fi
    grep -q 'only_read' "$tmp/out" || { echo "self-test FAILED: the key was not named"; exit 1; }
    echo '  ok: a key read but never written fails, by name'
    printf 's.setString("only_written", "x");\n' > "$tmp/src/b.cpp"
    if run; then echo "self-test FAILED: a write-only key passed"; exit 1; fi
    echo '  ok: a key written but never read fails'
    printf 'int y = s.getInt("ethernet_backend", 1);\n' > "$tmp/src/b.cpp"
    run || { cat "$tmp/out"; echo "self-test FAILED: an allowlisted key was refused"; exit 1; }
    echo '  ok: an allowlisted one-sided key passes'
    printf '// s.getInt("in_a_comment", 1);\n' > "$tmp/src/b.cpp"
    if run; then echo "self-test FAILED: a key in a comment was not seen"; exit 1; fi
    echo '  ok: commented-out calls still count (a guard should not be lenient)'
    rm "$tmp/src/b.cpp"; : > "$tmp/src/a.cpp"
    if run; then echo "self-test FAILED: an empty tree passed"; exit 1; fi
    echo '  ok: a tree with no keys at all fails'
    echo "check_settings_keys: self-test passed"
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit 0; fi

ROOT=${POM2_SETTINGS_ROOT:-"$(cd "$(dirname "$0")/.." && pwd)"}
scan() {  # scan get|set → sorted unique keys; calls may span lines
    find "$ROOT/src" \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) -type f -print0 \
        | xargs -0 perl -0777 -ne \
            'while (/\b'"$1"'(?:String|Bool|Int|Float|Double)\s*\(\s*"([A-Za-z0-9_]+)"/g) { print "$1\n" }' \
        | sort -u || true
}
READ=$(scan get)
WRITE=$(scan set)
if [ -z "$READ" ] && [ -z "$WRITE" ]; then
    echo "check_settings_keys: no settings calls found under $ROOT/src" >&2
    exit 2
fi
allowed() { printf '%s\n' "$ALLOW" | awk '{print $1}' | grep -qxF "$1"; }

bad=0
while IFS= read -r k; do
    [ -n "$k" ] || continue
    printf '%s\n' "$WRITE" | grep -qxF "$k" && continue
    allowed "$k" && continue
    echo "READ-ONLY   $k   (read with a default, written by nothing)"
    bad=1
done <<< "$READ"
while IFS= read -r k; do
    [ -n "$k" ] || continue
    printf '%s\n' "$READ" | grep -qxF "$k" && continue
    allowed "$k" && continue
    echo "WRITE-ONLY  $k   (written, read by nothing)"
    bad=1
done <<< "$WRITE"
# An allowlist entry nobody uses any more is a lie waiting to be believed.
[ -n "${POM2_SETTINGS_ROOT:-}" ] || while IFS= read -r line; do
    k=$(printf '%s' "$line" | awk '{print $1}')
    [ -n "$k" ] || continue
    if ! printf '%s\n%s\n' "$READ" "$WRITE" | grep -qxF "$k"; then
        echo "STALE ALLOW $k   (no longer read or written — drop it from the list)"
        bad=1
    fi
done <<< "$ALLOW"

if [ "$bad" != 0 ]; then
    echo "check_settings_keys: FAILED"
    exit 1
fi
echo "check_settings_keys: passed ($(printf '%s\n' "$READ" | grep -c .) read, $(printf '%s\n' "$WRITE" | grep -c .) written)"
