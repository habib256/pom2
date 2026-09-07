#!/usr/bin/env bash
# POM2 — line coverage over the test suite, with a floor.
#
# WHY. The 2026-08-28 architecture plan asserted that three subsystems were
# untested: TnfsClient, FujiNetNetDevice and FloppyEmuDevice. All three had
# test suites. Holes were being found by reading, and reading got it wrong
# three times out of three. This measures instead, and names the LINES.
#
# Clang source-based coverage, not gcov: it counts regions, so a half-taken
# `a && b` and an untaken `else` are visible rather than reading as covered.
#
#   tools/coverage.sh                # measure, report, check the floor
#   tools/coverage.sh --update       # re-record the floor from this run
#   tools/coverage.sh --html DIR     # also write a browsable report
#   tools/coverage.sh --self-test    # exercise the text guards, no build
#
# The floor may go UP freely; it may not go down. Same ratchet shape as
# tools/check_file_sizes.sh, and for the same reason: a rule with no
# mechanism measured -74 % on this repo (see that script's header).
#
# A percentage alone is not enough — see the "linked sources" guard below.

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${POM2_COVERAGE_BUILD:-build-coverage}"
FLOOR_FILE="tools/coverage_floor.txt"
LINKED_FILE="tools/coverage_linked_sources.txt"
UPDATE=0
SELF_TEST=0
HTML_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --update)    UPDATE=1 ;;
        --self-test) SELF_TEST=1 ;;
        --html)      shift; HTML_DIR="${1:-coverage-html}" ;;
        *) echo "usage: $0 [--update] [--html DIR] [--self-test]" >&2; exit 2 ;;
    esac
    shift
done

# ── The "linked sources" guard ────────────────────────────────────────────
#
# WHY a SECOND ratchet next to the percentage. The percentage is blind in two
# directions at once:
#
#   * it only ever sees the code the test binaries LINK. A source that no
#     test links contributes neither a covered nor a missed line — 42 of the
#     156 src/*.cpp files, ~25 850 lines, when this was written. The number
#     can be 90 % of a sixth of the program.
#   * it is therefore RAISABLE BY DELETING WORK. Drop a poorly covered source
#     from a test's SOURCES list and the measured percentage goes UP, the
#     floor is satisfied, and the ratchet has rewarded removing coverage.
#
# So the SET of linked first-party sources is recorded beside the floor and
# ratcheted the same way: it may grow freely, it may not shrink. A file that
# leaves the set fails the run and is named.
#
# The three functions below are pure text transforms on purpose — `--self-test`
# drives them from hand-made fixtures, with no build tree, no llvm-cov and no
# 4 GB of second build directory.

# stdin: an `llvm-cov report`. $1: repo root to strip from absolute paths.
# stdout: the sorted set of first-party sources, one `src/….cpp` per line.
# Headers are deliberately out: whether an inline-only header shows up in the
# report depends on which objects carried its mapping, which is not a signal
# about what the suite links.
cov_linked_from_report() {
    local root="${1:-$PWD}"
    awk -v root="$root/" '
        {
            p = $1
            sub(/^\.\//, "", p)
            if (index(p, root) == 1) p = substr(p, length(root) + 1)
            if (p ~ /^src\/.*\.cpp$/) print p
        }' | sort -u
}

# $1: a recorded list file. Comments (`#`) and blank lines are allowed so the
# file can explain itself; this is the set it denotes.
cov_recorded_set() {
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$1" | grep -v '^[[:space:]]*$' | sort -u
}

# $1 recorded, $2 measured. Prints the paths that LEFT the set (empty = ok).
cov_dropped_sources() {
    comm -23 <(cov_recorded_set "$1") <(sort -u "$2")
}

# True when the list was SEEDED BY HAND (from CMake's own source lists)
# rather than measured by a real coverage run. See the bootstrap below.
cov_seed_is_static() { grep -q '^# seeded-statically' "$1" 2>/dev/null; }

cov_write_linked_file() {   # $1 = measured list, $2 = destination
    {
        cat <<'EOF'
# The set of first-party sources (src/**.cpp) that the coverage run's binaries
# link, recorded by tools/coverage.sh. It is a RATCHET: it may grow freely, it
# may not shrink. A source that leaves the set fails the coverage job, because
# unlinking a poorly covered file RAISES the percentage — the floor alone
# rewards deleting coverage. Regenerate with `tools/coverage.sh --update`.
EOF
        cat "$1"
    } > "$2"
}

cov_self_test() {
    local tmp; tmp=$(mktemp -d) || return 2
    local fails=0
    _check() {  # $1 label, $2 expected, $3 actual
        if [ "$2" = "$3" ]; then
            printf '  ok    %s\n' "$1"
        else
            printf '  FAIL  %s\n       expected: [%s]\n       actual:   [%s]\n' \
                   "$1" "$2" "$3"
            fails=$((fails + 1))
        fi
    }

    # A report in llvm-cov's real shape: absolute paths, a header, a TOTAL.
    cat > "$tmp/report.txt" <<EOF
Filename                    Regions Missed Cover Functions Missed Executed Lines Missed Cover
/repo/src/Alpha.cpp             10      1 90.00%        3      0  100.00%    40      4 90.00%
/repo/src/sub/Beta.cpp           5      5  0.00%        2      2    0.00%    20     20  0.00%
/repo/src/Gamma.h                4      0 100.00%       1      0  100.00%     8      0 100.00%
/repo/tests/alpha_test.cpp      12      0 100.00%       4      0  100.00%    50      0 100.00%
/repo/third_party/x/zz.cpp       9      9  0.00%        1      1    0.00%    30     30  0.00%
TOTAL                           40     15 62.50%       11      3  72.72%   148     54 63.51%
EOF
    _check "report → linked src/*.cpp only" \
        "src/Alpha.cpp
src/sub/Beta.cpp" \
        "$(cov_linked_from_report /repo < "$tmp/report.txt")"

    # A report whose paths are already relative (some llvm-cov versions).
    printf 'src/Alpha.cpp 1 0 100%%\n./src/Delta.cpp 1 0 100%%\nTOTAL 1 0 100%%\n' \
        > "$tmp/rel.txt"
    _check "relative and ./-prefixed paths normalise" \
        "src/Alpha.cpp
src/Delta.cpp" \
        "$(cov_linked_from_report /repo < "$tmp/rel.txt")"

    # The recorded file may carry prose.
    cat > "$tmp/recorded.txt" <<'EOF'
# a comment
src/Alpha.cpp

src/Beta.cpp   # trailing note
src/Gamma.cpp
EOF
    _check "recorded set strips comments and blanks" \
        "src/Alpha.cpp
src/Beta.cpp
src/Gamma.cpp" \
        "$(cov_recorded_set "$tmp/recorded.txt")"

    printf 'src/Alpha.cpp\nsrc/Gamma.cpp\n' > "$tmp/measured_short.txt"
    _check "a source that left the set is named" \
        "src/Beta.cpp" \
        "$(cov_dropped_sources "$tmp/recorded.txt" "$tmp/measured_short.txt")"

    printf 'src/Alpha.cpp\nsrc/Beta.cpp\nsrc/Gamma.cpp\nsrc/Delta.cpp\n' \
        > "$tmp/measured_grown.txt"
    _check "a GROWN set is not a failure" \
        "" \
        "$(cov_dropped_sources "$tmp/recorded.txt" "$tmp/measured_grown.txt")"

    printf 'src/Alpha.cpp\nsrc/Beta.cpp\nsrc/Gamma.cpp\n' > "$tmp/measured_same.txt"
    _check "an unchanged set is not a failure" \
        "" \
        "$(cov_dropped_sources "$tmp/recorded.txt" "$tmp/measured_same.txt")"

    printf '# seeded-statically 2026-01-01\nsrc/Alpha.cpp\n' > "$tmp/seeded.txt"
    cov_seed_is_static "$tmp/seeded.txt"    && s1=yes || s1=no
    cov_seed_is_static "$tmp/recorded.txt"  && s2=yes || s2=no
    _check "the static-seed marker is detected"     "yes" "$s1"
    _check "a measured list is not a static seed"   "no"  "$s2"

    # The real file in the tree must parse, and must be non-empty.
    if [ -f "$LINKED_FILE" ]; then
        local n ok
        n=$(cov_recorded_set "$LINKED_FILE" | wc -l | tr -d ' ')
        if [ "$n" -gt 50 ]; then ok=yes; else ok=no; fi
        _check "$LINKED_FILE parses to a plausible set ($n entries)" "yes" "$ok"
        _check "every recorded path exists in the tree" "" \
            "$(cov_recorded_set "$LINKED_FILE" | while read -r f; do
                   [ -f "$f" ] || echo "$f"; done)"
    fi

    rm -rf "$tmp"
    if [ "$fails" -ne 0 ]; then
        echo "coverage --self-test: $fails check(s) failed" >&2
        return 1
    fi
    echo "coverage --self-test: all checks passed"
    return 0
}

if [ "$SELF_TEST" -eq 1 ]; then
    cov_self_test
    exit $?
fi

# The Xcode toolchain ships these but does not put them on PATH.
if command -v xcrun > /dev/null 2>&1; then
    PROFDATA=$(xcrun --find llvm-profdata 2>/dev/null || echo llvm-profdata)
    COV=$(xcrun --find llvm-cov 2>/dev/null || echo llvm-cov)
else
    PROFDATA="${LLVM_PROFDATA:-llvm-profdata}"
    COV="${LLVM_COV:-llvm-cov}"
fi
for t in "$PROFDATA" "$COV"; do
    command -v "$t" > /dev/null 2>&1 || {
        echo "coverage: $t not found — install the LLVM tools" >&2; exit 2; }
done

echo "coverage: configuring $BUILD_DIR"
# C is not enabled as a project language, so naming a C compiler only earns a
# "manually-specified variable was not used" warning.
mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DPOM2_ENABLE_TESTS=ON \
      -DPOM2_COVERAGE=ON \
      -DCMAKE_CXX_COMPILER=clang++ > "$BUILD_DIR/configure.log" 2>&1 || {
    echo "coverage: configure failed" >&2
    tail -30 "$BUILD_DIR/configure.log" >&2
    exit 1
}

echo "coverage: building"
cmake --build "$BUILD_DIR" --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      > "$BUILD_DIR/build.log" 2>&1 || {
    echo "coverage: build failed" >&2
    echo "--- compiler ---" >&2
    "${CXX:-clang++}" --version 2>&1 | head -2 >&2
    # The ERRORS, not the tail. A --parallel build ends with whatever finished
    # last, which is usually a target that succeeded, so a tail is exactly the
    # part of the log that does not say what went wrong. (This cost a CI round
    # trip the first time the job ran.)
    echo "--- errors ---" >&2
    grep -nE "error:|Error [0-9]|fatal error" "$BUILD_DIR/build.log" | head -40 >&2
    # A build that ran out of room says so in a way easy to miss among 200
    # parallel targets, and the coverage tree is big enough for that to be the
    # first thing to check.
    echo "--- disk ---" >&2
    df -h . >&2
    du -sh "$BUILD_DIR" 2>/dev/null >&2
    echo "--- context around the first error ---" >&2
    grep -n -B6 -A12 -m1 "error:" "$BUILD_DIR/build.log" >&2 || \
        tail -40 "$BUILD_DIR/build.log" >&2
    exit 1
}

PROF_DIR="$BUILD_DIR/profraw"
rm -rf "$PROF_DIR"; mkdir -p "$PROF_DIR"

# One .profraw per PROCESS (%p), not per test: several tests fork, and a
# shared filename would have the children stamp on each other's counters.
#
# pom2_core_sdk_consumer is excluded, and the reason is not "it is slow". It
# configures a SEPARATE cmake project against the installed POM2::core and
# links it with plain flags — against a coverage-instrumented archive, which
# needs -fprofile-instr-generate at link time and does not get it. Teaching
# the exported package about it would bake a build-mode flag into what
# consumers install. It measures the install/export contract, not POM2's
# code, so it has nothing to contribute to a coverage number anyway; the
# normal build/ run is where it belongs.
echo "coverage: running the suite"
( cd "$BUILD_DIR" && \
  LLVM_PROFILE_FILE="$PWD/profraw/pom2-%p.profraw" \
  ctest --output-on-failure -E '^pom2_core_sdk_consumer$' \
        -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
  > ctest.log 2>&1 )
CTEST_RC=$?
if [ "$CTEST_RC" -ne 0 ]; then
    echo "coverage: the suite is not green — measure on a green tree" >&2
    # ctest names the casualties as "  N - name (Failed)" / "(Timeout)" —
    # lower-case, so a grep for FAILED prints the "The following tests
    # FAILED:" banner and then nothing. CI's log is all anyone gets (this
    # build tree is not uploaded), so a red run that does not name the test
    # is a run nobody can act on. Print the banner AND what follows it, plus
    # the failing tests' own output.
    grep -E "tests passed|\(Failed\)|\(Timeout\)|\(Subprocess aborted\)" \
         "$BUILD_DIR/ctest.log" >&2
    # …and WHY. `tail` shows the end of the log, but a test that fails at
    # number 44 of 246 had its output scrolled past 200 tests ago — which is
    # how a red coverage run came back naming two casualties and giving no
    # reason for either. Re-run just the casualties, serially, and let their
    # own output through.
    cov_failed_names=$(sed -n 's/^[[:space:]]*[0-9][0-9]* - \([^ ]*\) (\(Failed\|Timeout\|Subprocess aborted\|Not Run\)).*/\1/p' \
                       "$BUILD_DIR/ctest.log" | sort -u)
    if [ -n "$cov_failed_names" ]; then
        cov_re="^($(echo "$cov_failed_names" | paste -sd'|' -))$"
        echo "--- re-running the failures with their output: $cov_re ---" >&2
        ( cd "$BUILD_DIR" && ctest --output-on-failure -R "$cov_re" >&2 ) || true
    fi
    echo "--- last 80 lines of ctest.log ---" >&2
    tail -80 "$BUILD_DIR/ctest.log" >&2
    exit 1
fi

shopt -s nullglob
RAW=( "$PROF_DIR"/*.profraw )
if [ ${#RAW[@]} -eq 0 ]; then
    echo "coverage: no .profraw produced — is POM2_COVERAGE really on?" >&2
    exit 2
fi
echo "coverage: merging ${#RAW[@]} profiles"
"$PROFDATA" merge -sparse -o "$BUILD_DIR/pom2.profdata" "${RAW[@]}" || exit 1

# WHAT IS MEASURED: the code the TEST SUITE LINKS, not the whole program.
#
# Reporting against the test binaries rather than the GUI one is a deliberate
# scope, and the alternative is worse. `POM2` links the ImGui frontend, which
# no headless test can exercise, so including it would put ~15 000 unreachable
# lines in the denominator, report ~27 %, and make the floor a measure of how
# much UI exists rather than of how well anything is tested.
#
# So the number answers: "of the code POM2's tests are built against, how much
# do they actually run?" That is the question a ratchet can act on.
#
# llvm-cov wants the archive members that carry the coverage mapping, and an
# Apple `.a` does not hand them over — the executables do.
#
# Every EXECUTABLE under tests/, not `test_*`: the glob used to be `test_*`,
# which silently dropped anything ctest runs under another name. `pom2_headless`
# is the live example — `headless_boot_capture` (tests/CMakeLists.txt:6360)
# runs it for 300 frames, its counters land in the merged profile, and its
# objects were never handed to llvm-cov, so `src/pom2_headless.cpp` and
# everything only it links were invisible. It does not even live under tests/,
# so it is named explicitly. The probe binaries are EXCLUDE_FROM_ALL and simply
# are not built here; `nullglob` + the -f/-x tests skip directories, the two
# `.bin` fixtures and the logs.
OBJS=()
cov_add_object() { [ -f "$1" ] && [ -x "$1" ] && OBJS+=(-object "$1"); }
for f in "$BUILD_DIR"/tests/*; do
    case "$f" in
        *.bin|*.cmake|*.log|*.txt|*.ppm|*.json|*.profraw) continue ;;
    esac
    cov_add_object "$f"
done
cov_add_object "$BUILD_DIR/pom2_headless"
if [ ${#OBJS[@]} -eq 0 ]; then
    echo "coverage: no test binaries in $BUILD_DIR/tests" >&2
    exit 2
fi
# The first -object is positional for llvm-cov; the rest keep their flag.
OBJS=("${OBJS[@]:1}")

# Vendored, generated, and the tests' own bodies are not ours to cover.
IGNORE='(third_party/|stb_image|imgui/|/tests/|Ssi263PhonemeData|ImageWriterRom|AppleIIeKeyboardLayout)'

REPORT=$("$COV" report "${OBJS[@]}" -instr-profile="$BUILD_DIR/pom2.profdata" \
         -ignore-filename-regex="$IGNORE" 2>/dev/null)
SUMMARY=$(printf '%s\n' "$REPORT" | grep '^TOTAL')
[ -n "$SUMMARY" ] || { echo "coverage: llvm-cov produced no summary" >&2; exit 2; }

# llvm-cov's columns: Regions/Missed/Cover, Functions/Missed/Executed,
# Lines/Missed/Cover, Branches/Missed/Cover. The THIRD percentage is lines.
pct3() { awk '{ n=0; for (i=1;i<=NF;i++) if ($i ~ /%$/) { n++; if (n==3) { gsub(/%/,"",$i); print $i; exit } } }'; }
LINES_PCT=$(printf '%s\n' "$SUMMARY" | pct3)
[ -n "$LINES_PCT" ] || { echo "coverage: could not read the line column" >&2; exit 2; }

if [ -n "$HTML_DIR" ]; then
    "$COV" show "${OBJS[@]}" -instr-profile="$BUILD_DIR/pom2.profdata" \
        -ignore-filename-regex="$IGNORE" -format=html -output-dir="$HTML_DIR" \
        > /dev/null 2>&1 && echo "coverage: HTML report in $HTML_DIR/index.html"
fi

echo ""
echo "coverage: line coverage ${LINES_PCT}%  (over the code the tests link)"
echo ""
echo "Least-covered first-party files — this is the list the number is for:"
printf '%s\n' "$REPORT" \
  | grep -E '\.(cpp|h)[[:space:]]' \
  | awk '{ n=0; for (i=1;i<=NF;i++) if ($i ~ /%$/) { n++; if (n==3) { gsub(/%/,"",$i); printf "%7.2f%%  %s\n", $i, $1; break } } }' \
  | sort -n | head -15 | sed 's/^/  /'

# The measured set of linked first-party sources — the other half of the
# ratchet (see the guard's header above).
MEASURED_LINKED="$BUILD_DIR/linked_sources.txt"
printf '%s\n' "$REPORT" | cov_linked_from_report "$PWD" > "$MEASURED_LINKED"
MEASURED_N=$(wc -l < "$MEASURED_LINKED" | tr -d ' ')
if [ "$MEASURED_N" -eq 0 ]; then
    echo "coverage: llvm-cov named no src/*.cpp — the -object list is wrong" >&2
    exit 2
fi
echo "coverage: the suite links ${MEASURED_N} first-party sources"

if [ "$UPDATE" -eq 1 ]; then
    cov_write_linked_file "$MEASURED_LINKED" "$LINKED_FILE"
    echo "coverage: recorded ${MEASURED_N} linked sources in $LINKED_FILE"
    # Recorded HALF A POINT BELOW what was measured, on purpose. Two runs of
    # the same tree differ by ~0.1 % — tests that fork, a timing-shaped case
    # that takes a different branch — and a floor pinned to the exact reading
    # would fail on noise, which is how a ratchet gets switched off. Half a
    # point absorbs that and still catches a real regression: adding one
    # untested file moves the number by far more.
    MARGIN=$(awk -v v="$LINES_PCT" 'BEGIN { m = v - 0.5; if (m < 0) m = 0; printf "%.2f", m }')
    printf '%s\n' "$MARGIN" > "$FLOOR_FILE"
    echo ""
    echo "coverage: measured ${LINES_PCT}%, floor recorded at ${MARGIN}%"
    echo "          (half a point of margin — see the note in this script)"
    exit 0
fi

if [ ! -f "$LINKED_FILE" ]; then
    echo "coverage: $LINKED_FILE is missing — run '$0 --update'" >&2
    exit 2
fi
if cov_seed_is_static "$LINKED_FILE"; then
    # ONE-TIME BOOTSTRAP. The list shipped with this change was seeded by hand
    # from CMake's own per-target source lists (every binary ctest invokes),
    # because the person adding the guard could not run a coverage build — it
    # configures a second ~4 GB tree. That seed is close but not authoritative:
    # llvm-cov reports the files whose mapping is actually in the objects, so a
    # source the seed lists but the report does not is a seeding artefact, not a
    # regression, and failing on it would only teach people to delete the file.
    # The first real run therefore REPLACES the seed with the truth and passes,
    # loudly. Every run after that is a plain ratchet.
    SEED_N=$(cov_recorded_set "$LINKED_FILE" | wc -l | tr -d ' ')
    cov_write_linked_file "$MEASURED_LINKED" "$LINKED_FILE"
    echo "coverage: replaced the hand-seeded linked-source list"
    echo "          (seeded ${SEED_N} from CMake, measured ${MEASURED_N})"
    echo "          COMMIT $LINKED_FILE — the ratchet starts at the next run."
    # CI's workspace is thrown away, so the rewritten file only exists in this
    # log. Print it: that is what a human copies into the repo to arm the
    # ratchet. It is ~100 lines, once.
    echo "--- $LINKED_FILE as measured ---"
    cat "$LINKED_FILE"
    echo "--- end ---"
    DROPPED=$(printf '')
else
    DROPPED=$(cov_dropped_sources "$LINKED_FILE" "$MEASURED_LINKED")
fi
if [ -n "$DROPPED" ]; then
    echo "" >&2
    echo "FAIL  these sources left the set the tests link:" >&2
    printf '%s\n' "$DROPPED" | sed 's/^/        /' >&2
    echo "      Unlinking a file RAISES the percentage, so the floor cannot" >&2
    echo "      catch this. Put them back on a test's SOURCES, or remove them" >&2
    echo "      from $LINKED_FILE and say why in the commit." >&2
    exit 1
fi

if [ ! -f "$FLOOR_FILE" ]; then
    echo "coverage: $FLOOR_FILE is missing — run '$0 --update'" >&2
    exit 2
fi
FLOOR=$(head -1 "$FLOOR_FILE")
echo ""
# Integer compare on tenths: bash has no floats, and a floor to 0.1 % is
# finer than the noise between two runs of the same tree.
cur=$(printf '%.0f' "$(echo "$LINES_PCT" | awk '{print $1*10}')")
flr=$(printf '%.0f' "$(echo "$FLOOR"     | awk '{print $1*10}')")
if [ "$cur" -lt "$flr" ]; then
    echo "FAIL  line coverage ${LINES_PCT}% is below the floor ${FLOOR}%" >&2
    echo "      Add tests for what you changed, or lower the floor in" >&2
    echo "      $FLOOR_FILE and say why in the commit." >&2
    exit 1
fi
echo "coverage: passed (floor ${FLOOR}%)"
exit 0
