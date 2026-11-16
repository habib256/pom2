#!/usr/bin/env bash
# check_ctest_inventory.sh <build-dir> — fail if a test listed in
# tools/ctest_registered.txt is not registered with ctest.
#
# The count floor alone is blind to a whole block of tests disappearing: a
# failed oracle download unregisters Klaus or zexall, a ROM missing at
# configure unregisters the headless captures, a dropped target unregisters
# ~80 tests at once, and 490 - 80 is still above 300. A test that RUNS and
# skips is check_ctest_skips.sh's job; this one catches the test that is not
# there at all. Format and rules: the header of tools/ctest_registered.txt.
#
# Usage: tools/check_ctest_inventory.sh <build-dir> [<list>]
#        tools/check_ctest_inventory.sh --self-test
set -euo pipefail
export LC_ALL=C

here=$(cd "$(dirname "$0")" && pwd)

platform() {
    case "$(uname -s)" in
        Linux)  echo linux ;;
        Darwin) echo macos ;;
        *)      echo other ;;
    esac
}

# required <list> <platform> → the names that platform must register, sorted.
required() {
    awk -v plat="$2" '
        /^[ \t]*#/ || NF == 0 { next }
        NF == 1 || $2 == plat { print $1 }
    ' "$1" | sort -u
}

# check <list> <registered-names-file> <platform> → missing names on stdout,
# status 1 when any is missing.
check() {
    local missing
    missing=$(comm -23 <(required "$1" "$3") <(sort -u "$2"))
    if [ -n "$missing" ]; then
        printf '%s\n' "$missing"
        return 1
    fi
}

self_test() {
    local tmp; tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    cat > "$tmp/list" <<'EOF'
# comment
klaus_6502_functional
z80_zexall

native_file_dialog linux
mac_only macos
EOF
    printf '%s\n' klaus_6502_functional z80_zexall native_file_dialog \
        mac_only extra_test > "$tmp/all"
    # Everything present, on both platforms: passes (an unlisted extra is fine).
    check "$tmp/list" "$tmp/all" linux >/dev/null || { echo "self-test: full set refused (linux)"; return 1; }
    check "$tmp/list" "$tmp/all" macos >/dev/null || { echo "self-test: full set refused (macos)"; return 1; }
    # A missing oracle is reported, by name — the negative control.
    grep -v '^z80_zexall$' "$tmp/all" > "$tmp/nozex"
    local out
    if out=$(check "$tmp/list" "$tmp/nozex" linux); then
        echo "self-test: a missing z80_zexall passed"; return 1
    fi
    [ "$out" = "z80_zexall" ] || { echo "self-test: reported '$out', not z80_zexall"; return 1; }
    # A platform-tagged name binds only its platform.
    grep -v '^native_file_dialog$' "$tmp/all" > "$tmp/nonfd"
    check "$tmp/list" "$tmp/nonfd" macos >/dev/null || { echo "self-test: linux-only name required on macos"; return 1; }
    if check "$tmp/list" "$tmp/nonfd" linux >/dev/null; then
        echo "self-test: linux-only name not required on linux"; return 1
    fi
    # The real list parses and is not empty.
    [ "$(required "$here/ctest_registered.txt" linux | wc -l)" -gt 400 ] ||
        { echo "self-test: tools/ctest_registered.txt lists too few names"; return 1; }
    echo "check_ctest_inventory self-test OK"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

build=${1:?usage: check_ctest_inventory.sh <build-dir> [<list>] | --self-test}
list=${2:-$here/ctest_registered.txt}
names=$(mktemp)
trap 'rm -f "$names"' EXIT
(cd "$build" && ctest -N) | awk '/^ *Test +#[0-9]+: /{print $3}' > "$names"

plat=$(platform)
want=$(required "$list" "$plat" | wc -l | tr -d ' ')
if missing=$(check "$list" "$names" "$plat"); then
    echo "ctest inventory: all $want listed tests registered ($plat, $(wc -l < "$names" | tr -d ' ') in all)"
    exit 0
fi
n=$(printf '%s\n' "$missing" | wc -l | tr -d ' ')
echo "::error::$n test(s) listed in tools/ctest_registered.txt are not registered with ctest ($plat):"
printf '  %s\n' $missing
echo "A configure-time download (Klaus, zexall, Tom Harte), a ROM missing at"
echo "configure (headless_*), or a dropped target or add_test() removed them."
exit 1
