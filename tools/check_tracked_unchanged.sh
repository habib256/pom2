#!/usr/bin/env bash
# check_tracked_unchanged.sh — the test suite leaves the source tree as it
# found it.
#
#   tools/check_tracked_unchanged.sh --record  <state-file>   # before ctest
#   tools/check_tracked_unchanged.sh --compare <state-file>   # after: fails on a change
#   tools/check_tracked_unchanged.sh --self-test              # negative controls
#
# Why. Media write by default (CLAUDE.md), and dozens of tests boot the
# tracked images: a card commits a dirty disk in its destructor, the floppy
# autosave a second after the drive goes quiet. The rule is that such a test
# copies the image first — and ctest sets POM2_MEDIA_WRITE_DEFAULT=protected
# as the backstop — but nothing CHECKED it: a test that wrote into
# disks_5.4/DIRTBIKE.dsk left a green suite and a modified tree, and the
# next commit carried the damage. The same goes for a probe or a test that
# drops its output in the working directory instead of under
# pom2test::probeOutDir().
#
# What is compared: `git status --porcelain` (tracked changes AND untracked,
# non-ignored files) plus a hash of `git diff HEAD`, so a tree that was
# already dirty before the run passes as long as the run does not touch it
# further. Ignored paths (build/, the fetched ROMs and media) are not looked
# at. Plain bash 3.2 and git only: the macOS leg runs it too.
set -euo pipefail
export LC_ALL=C

state() {
    # `git -C "$1"`: the tree to look at. Each part on its own line so the
    # failure message can show what changed.
    git -C "$1" status --porcelain=v1 --untracked-files=all
    printf 'diff %s\n' "$(git -C "$1" diff HEAD --binary | git hash-object --stdin)"
}

root_of() { git -C "${1:-.}" rev-parse --show-toplevel; }

record() {
    local out=$1 root
    root=$(root_of .)
    state "$root" > "$out"
    echo "check_tracked_unchanged: recorded the tree state ($(grep -vc '^diff ' "$out" || true) pending path(s))"
}

compare() {
    local before=$1 root now
    root=$(root_of .)
    now=$(mktemp)
    state "$root" > "$now"
    if cmp -s "$before" "$now"; then
        rm -f "$now"
        echo "check_tracked_unchanged: passed — the run modified nothing in the tree"
        return 0
    fi
    echo "::error::the test run modified the source tree (a test wrote a tracked file, or left a file outside build/):"
    diff "$before" "$now" | grep '^[<>]' | grep -v ' diff ' | sed 's/^/  /' || true
    echo "  git diff --stat:"
    git -C "$root" diff --stat HEAD | sed 's/^/    /'
    echo "A test that boots a tracked image must copy it first (CLAUDE.md, media write"
    echo "by default); a probe writes under pom2test::probeOutDir(), never the cwd."
    rm -f "$now"
    return 1
}

self_test() {
    local state_file rc
    SELF_TEST_DIR=$(mktemp -d)
    trap 'rm -rf "$SELF_TEST_DIR"' EXIT
    local dir=$SELF_TEST_DIR
    git -C "$dir" init -q
    git -C "$dir" config user.email t@example.invalid
    git -C "$dir" config user.name test
    printf 'disk\n' > "$dir/image.dsk"
    printf 'build/\n' > "$dir/.gitignore"
    git -C "$dir" add -A && git -C "$dir" commit -qm init
    mkdir -p "$dir/build"
    state_file="$dir/build/state"

    # 1. Nothing touched: passes.
    (cd "$dir" && record "$state_file" >/dev/null && compare "$state_file" >/dev/null) ||
        { echo "self-test FAIL: an untouched tree was reported changed"; return 1; }
    # 2. A file under an ignored directory (build/): passes.
    (cd "$dir" && record "$state_file" >/dev/null && printf 'log' > build/ctest.log &&
        compare "$state_file" >/dev/null) ||
        { echo "self-test FAIL: an ignored build output was reported"; return 1; }
    # 3. A tracked image written: fails.
    rc=0
    (cd "$dir" && record "$state_file" >/dev/null && printf 'guest wrote\n' >> image.dsk &&
        compare "$state_file" >/dev/null) || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test FAIL: a written tracked file passed"; return 1; }
    git -C "$dir" checkout -q -- image.dsk
    # 4. An untracked file dropped in the tree: fails.
    rc=0
    (cd "$dir" && record "$state_file" >/dev/null && printf 'x' > probe.ppm &&
        compare "$state_file" >/dev/null) || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test FAIL: a stray untracked file passed"; return 1; }
    rm -f "$dir/probe.ppm"
    # 5. Already dirty before the run, untouched by it: passes.
    printf 'local edit\n' >> "$dir/image.dsk"
    (cd "$dir" && record "$state_file" >/dev/null && compare "$state_file" >/dev/null) ||
        { echo "self-test FAIL: a tree dirty BEFORE the run was blamed on it"; return 1; }
    # 6. Already dirty, then written AGAIN by the run (same path, same status
    #    letter — only the diff hash tells): fails.
    rc=0
    (cd "$dir" && record "$state_file" >/dev/null && printf 'again\n' >> image.dsk &&
        compare "$state_file" >/dev/null) || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test FAIL: a second write to a dirty file passed"; return 1; }
    echo "check_tracked_unchanged: self-test passed (6 cases)"
}

case "${1:-}" in
    --record)    [ $# -eq 2 ] || { echo "usage: $0 --record <state-file>" >&2; exit 2; }; record "$2" ;;
    --compare)   [ $# -eq 2 ] || { echo "usage: $0 --compare <state-file>" >&2; exit 2; }; compare "$2" ;;
    --self-test) self_test ;;
    *) echo "usage: $0 --record <file> | --compare <file> | --self-test" >&2; exit 2 ;;
esac
