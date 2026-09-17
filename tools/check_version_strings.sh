#!/usr/bin/env bash
# check_version_strings.sh — the prose that cannot #include Version.h agrees
# with CMakeLists.txt's `project(pom2_imgui VERSION x.y.z)`.
#
# Compiled code reads the version from the generated header, and release.yml
# refuses a tag that differs from PROJECT_VERSION. Nothing checked the rest,
# which CLAUDE.md § Version string locations lists as "manual": vcpkg.json sat
# at "0.8" through three releases, and README carries the version in every
# package filename a user copies. So, for version V:
#   · vcpkg.json            "version-string": "V"
#   · CLAUDE.md             Current release: **vV**
#   · README.md             the title (`POM2 vV`), every package filename
#                           (`POM2-vV-…`, `POM2-<Platform>-vV.…`) and the
#                           `git tag vV` example;
#   · docs/releases/        vV.md or vMAJOR.MINOR.md exists — otherwise the
#                           GitHub Release body silently falls back to a
#                           generated commit list.
# A future version named in prose ("ships since v0.9.4") is NOT checked: only
# the shapes above claim to BE the current release.
#
# Usage: tools/check_version_strings.sh [--self-test]
# POM2_VERSION_ROOT overrides the tree to check (the self-test uses it).
set -euo pipefail
# Byte-wise: README starts with an emoji, and a UTF-8 locale on macOS makes
# sed give up on it silently.
export LC_ALL=C

self_test() {
    local tmp here
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    here="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    mk() {  # a consistent tree at version $1
        rm -rf "$tmp/t"; mkdir -p "$tmp/t/docs/releases"
        printf 'project(pom2_imgui VERSION %s LANGUAGES CXX)\n' "$1" > "$tmp/t/CMakeLists.txt"
        printf '{\n  "version-string": "%s"\n}\n' "$1" > "$tmp/t/vcpkg.json"
        printf 'Current release: **v%s**. Single source\n' "$1" > "$tmp/t/CLAUDE.md"
        printf '<div align="center">\n\n# \xf0\x9f\x8d\x8f POM2 v%s \xe2\x80\x94 Apple II Emulator\n| `POM2-v%s-x86_64.AppImage` |\n| `POM2-macOS-v%s.dmg` |\ngit tag v%s && git push\nships since v9.9.9\n' \
            "$1" "$1" "$1" "$1" > "$tmp/t/README.md"
        : > "$tmp/t/docs/releases/v$1.md"
    }
    expect() {  # expect <0|1> <label>
        local rc=0
        POM2_VERSION_ROOT="$tmp/t" bash "$here" > "$tmp/out" 2>&1 || rc=$?
        if { [ "$1" = 0 ] && [ "$rc" != 0 ]; } || { [ "$1" = 1 ] && [ "$rc" = 0 ]; }; then
            echo "self-test FAILED: $2 (exit $rc)"; cat "$tmp/out"; exit 1
        fi
        echo "  ok: $2"
    }
    mk 1.2.3; expect 0 "a consistent tree passes (a future version in prose is ignored)"
    mk 1.2.3; sed -i.bak 's/1.2.3/1.2.2/' "$tmp/t/vcpkg.json"; expect 1 "stale vcpkg.json"
    mk 1.2.3; sed -i.bak 's/v1.2.3/v1.2.2/' "$tmp/t/CLAUDE.md"; expect 1 "stale CLAUDE.md"
    mk 1.2.3; sed -i.bak '/^# /s/v1.2.3/v1.2.2/' "$tmp/t/README.md"; expect 1 "stale README title"
    mk 1.2.3; sed -i.bak 's/POM2-macOS-v1.2.3/POM2-macOS-v1.2.2/' "$tmp/t/README.md"; expect 1 "one stale package filename"
    mk 1.2.3; sed -i.bak 's/git tag v1.2.3/git tag v1.2.2/' "$tmp/t/README.md"; expect 1 "stale tag example"
    mk 1.2.3; rm "$tmp/t/docs/releases/v1.2.3.md"; expect 1 "missing release notes"
    mk 1.2.3; rm "$tmp/t/docs/releases/v1.2.3.md"; : > "$tmp/t/docs/releases/v1.2.md"
    expect 0 "vMAJOR.MINOR.md release notes are accepted"
    mk 1.2.3; rm "$tmp/t/CLAUDE.md"; expect 1 "a missing file is a failure, not a pass"
    echo "check_version_strings: self-test passed"
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit 0; fi

ROOT=${POM2_VERSION_ROOT:-"$(cd "$(dirname "$0")/.." && pwd)"}
cd "$ROOT"

V=$(sed -n 's/^project(pom2_imgui VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
if [ -z "$V" ]; then
    echo "check_version_strings: cannot read project(VERSION) from CMakeLists.txt" >&2
    exit 2
fi
for f in vcpkg.json CLAUDE.md README.md; do
    [ -f "$f" ] || { echo "check_version_strings: $f is missing" >&2; exit 2; }
done

bad=0
mismatch() { echo "STALE  $1: says $2, CMakeLists.txt says $V"; bad=1; }

got=$(sed -n 's/.*"version-string"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' vcpkg.json)
[ "$got" = "$V" ] || mismatch vcpkg.json "${got:-nothing}"

got=$(sed -n 's/.*Current release: \*\*v\([0-9.]*\)\*\*.*/\1/p' CLAUDE.md)
[ "$got" = "$V" ] || mismatch CLAUDE.md "${got:-nothing}"

# The first Markdown heading, wherever it sits (an HTML block may precede it).
got=$(grep -m1 '^# ' README.md | sed -n 's/.*POM2 v\([0-9.]*[0-9]\).*/\1/p' || true)
[ "$got" = "$V" ] || mismatch "README.md title" "${got:-nothing}"

while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    ver=${hit##*v}
    [ "$ver" = "$V" ] || mismatch "README.md ($hit)" "$ver"
done < <(grep -oE 'POM2-([A-Za-z]+-)?v[0-9]+(\.[0-9]+)+|git tag v[0-9]+(\.[0-9]+)+' README.md \
         | sed 's/\.$//' || true)

MM=$(printf '%s' "$V" | cut -d. -f1-2)
if [ ! -f "docs/releases/v$V.md" ] && [ ! -f "docs/releases/v$MM.md" ]; then
    echo "MISSING docs/releases/v$V.md (or v$MM.md) — the Release body would be a generated commit list"
    bad=1
fi

if [ "$bad" != 0 ]; then
    echo "check_version_strings: FAILED (see CLAUDE.md § Version string locations)"
    exit 1
fi
echo "check_version_strings: passed (v$V)"
