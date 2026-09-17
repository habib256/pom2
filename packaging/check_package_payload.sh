#!/usr/bin/env bash
#
# check_package_payload.sh <build-dir> — the two manifest consumers that
# `stage_data.sh --self-test` cannot reach, run for real.
#
# packaging/bundle.manifest is read by three parsers: stage_data.sh (.app and
# .zip), CMake's install() rules (every `cmake --install`, the AppImages and
# the .deb) and the emcc --preload-file / --exclude-file list (the browser
# bundle). The self-test proves the first. This proves the other two:
#
#   1. CMake parsed the manifest the way stage_data.sh does — the configure
#      dumps <build>/bundle_manifest.parsed, compared line for line;
#   2. `cmake --install` of this build tree, with leaks PLANTED in the source
#      payload (a differently-cased archive, an archive-named folder, a denied
#      folder nested inside), yields a tree `stage_data.sh --verify` accepts,
#      and the planted control file did land (so the absence is not vacuous);
#   3. the emcc exclude patterns CMake computed, run through Emscripten's own
#      packager walk (`tools/file_packager.py`, `add()`), drop the same leaks
#      and keep the control. With no Emscripten on the host the walk is a
#      line-for-line copy of emsdk 6.0.8's, and the log says so. A run with
#      NO patterns must ship the leaks — the negative control. Known blind
#      spot, found by mutation: that walk tests directories as well as files,
#      so dropping the `<glob>/*` directory form of a denyglob changes nothing
#      here — the file form already prunes an archive-named folder.
#
# Plants into the source tree (install(DIRECTORY) copies the working tree at
# install time — there is no other way) under `.pom2-paytest-*` names, removed
# on every exit path. The ctest holds RESOURCE_LOCK source_payload with
# `bundle_manifest`, which plants too.
set -euo pipefail

BUILD="${1:?usage: check_package_payload.sh <build-dir>}"
BUILD="$(cd "$BUILD" && pwd)"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="$REPO_ROOT/packaging/stage_data.sh"
PARSED="$BUILD/bundle_manifest.parsed"

die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

[ -f "$PARSED" ] || die "$PARSED is missing — reconfigure the build"

# ── 1. parser parity ───────────────────────────────────────────────────────
if ! diff <(grep -vE '^(wasm-exclude|datadir)'$'\t' "$PARSED") \
          <(bash "$STAGE" --list parsed) >/dev/null; then
    diff <(grep -vE '^(wasm-exclude|datadir)'$'\t' "$PARSED") \
         <(bash "$STAGE" --list parsed) >&2 || true
    die "CMake and stage_data.sh parse packaging/bundle.manifest differently"
fi
log "OK: CMake and stage_data.sh agree on the manifest"

FIRST_DIR=$(awk -F'\t' '$1=="dir"{print $2; exit}' "$PARSED")
DENY_ONE=$(awk -F'\t' '$1=="deny"{print $2; exit}' "$PARSED")
GLOB_ONE=$(awk -F'\t' '$1=="denyglob"{print $2; exit}' "$PARSED")
DATADIR=$(awk -F'\t' '$1=="datadir"{print $2; exit}' "$PARSED")
[ -n "$FIRST_DIR" ] && [ -n "$DENY_ONE" ] && [ -n "$GLOB_ONE" ] && [ -n "$DATADIR" ] \
    || die "the manifest needs a dir, a deny and a denyglob entry for this test"
[ -d "$REPO_ROOT/$FIRST_DIR" ] || die "$FIRST_DIR/ is missing from the source tree"

# ── plant ──────────────────────────────────────────────────────────────────
EXT="${GLOB_ONE#\*}"                                     # "*.zip" -> ".zip"
UPPER_EXT=$(printf '%s' "$EXT" | tr '[:lower:]' '[:upper:]')
P="$REPO_ROOT/$FIRST_DIR"
KEEP=".pom2-paytest-keep.bin"
LEAK_FILE=".pom2-paytest-file$UPPER_EXT"
LEAK_DIR=".pom2-paytest-dir$EXT"
LEAK_NEST=".pom2-paytest-nest"
DENY_NAME=$(basename "$DENY_ONE")
TMP="$(mktemp -d)"
cleanup() {
    rm -rf "$TMP" "${P:?}/$KEEP" "${P:?}/$LEAK_FILE" "${P:?}/$LEAK_DIR" "${P:?}/$LEAK_NEST"
}
trap cleanup EXIT
printf 'keep' > "$P/$KEEP"
printf 'leak' > "$P/$LEAK_FILE"
mkdir -p "$P/$LEAK_DIR" "$P/$LEAK_NEST/$DENY_NAME"
printf 'leak' > "$P/$LEAK_DIR/inside.bin"
printf 'leak' > "$P/$LEAK_NEST/$DENY_NAME/inside.bin"

# ── 2. cmake --install ─────────────────────────────────────────────────────
if ! cmake --install "$BUILD" --prefix "$TMP/prefix" > "$TMP/install.log" 2>&1; then
    cat "$TMP/install.log" >&2
    die "cmake --install failed"
fi
ROOT="$TMP/prefix/$DATADIR"
bash "$STAGE" --verify "$ROOT" > "$TMP/verify.log" 2>&1 \
    || { cat "$TMP/verify.log" >&2; die "the installed payload does not verify"; }
[ -f "$ROOT/$FIRST_DIR/$KEEP" ] \
    || die "the control file did not install — the leak checks below would prove nothing"
for leak in "$LEAK_FILE" "$LEAK_DIR" "$LEAK_NEST/$DENY_NAME"; do
    [ ! -e "$ROOT/$FIRST_DIR/$leak" ] || die "cmake --install shipped $FIRST_DIR/$leak"
done
log "OK: cmake --install prunes every planted leak and verifies ($DATADIR)"

# ── 3. the emcc walk ───────────────────────────────────────────────────────
EMROOT=""
if command -v em-config >/dev/null 2>&1; then
    EMROOT=$(em-config EMSCRIPTEN_ROOT 2>/dev/null || true)
fi
awk -F'\t' '$1=="wasm-exclude"{print $2}' "$PARSED" > "$TMP/patterns"
awk -F'\t' '$1=="dir"||$1=="wasm"{print $2}' "$PARSED" > "$TMP/roots"

python3 - "$REPO_ROOT" "$FIRST_DIR" "$EMROOT" "$TMP/patterns" "$TMP/roots" \
          "$KEEP" "$LEAK_FILE" "$LEAK_DIR" "$LEAK_NEST/$DENY_NAME" <<'PY'
import fnmatch, os, sys

repo, first, emroot, pat_file, roots_file, keep, *leaks = sys.argv[1:]
patterns = [l.rstrip("\n") for l in open(pat_file) if l.strip()]
roots = [l.rstrip("\n") for l in open(roots_file) if l.strip()]

packager = None
if emroot:
    try:
        sys.path.insert(0, emroot)
        from tools import file_packager as packager  # emsdk's own walk
    except Exception as e:  # an emsdk we cannot drive: say so, do not hide it
        print(f"note: could not import Emscripten's file_packager ({e})")
        packager = None

def walk(patterns):
    """Destination paths the packager would preload for every root."""
    if packager is not None:
        packager.excluded_patterns[:] = patterns
        packager.new_data_files.clear()
        packager.walked.clear()
        for r in roots:
            src = os.path.join(repo, r)
            if os.path.isdir(src):
                packager.add("preload", src, "/" + r)
        return {f.dstpath for f in packager.new_data_files}
    # emsdk 6.0.8 tools/file_packager.py add() + should_ignore(), verbatim in
    # behaviour: directories AND files are tested against every pattern,
    # with fnmatch on the full source path.
    def ignored(full):
        out = False
        for p in patterns:
            if p.startswith("!"):
                if fnmatch.fnmatch(full, p[1:]): out = False
            elif fnmatch.fnmatch(full, p):
                out = True
        return out
    got = set()
    for r in roots:
        src = os.path.join(repo, r)
        if not os.path.isdir(src):
            continue
        for dirpath, dirnames, filenames in os.walk(src):
            dirnames[:] = [d for d in dirnames
                           if not ignored(os.path.join(dirpath, d))]
            for n in filenames:
                full = os.path.join(dirpath, n)
                if not ignored(full):
                    got.add(os.path.join("/" + r, os.path.relpath(full, src)))
    return got

which = "Emscripten's own file_packager" if packager else "a copy of emsdk 6.0.8's walk (no Emscripten here)"
shipped = walk(patterns)
base = "/" + first + "/"
if base + keep not in shipped:
    sys.exit(f"FAIL: the WASM walk dropped the control file {keep}")
bad = [l for l in leaks if any(p == base + l or p.startswith(base + l + "/") for p in shipped)]
if bad:
    sys.exit(f"FAIL: POM2.data would ship {bad} ({which})")
naked = walk([])
if not all(any(p == base + l or p.startswith(base + l + "/") for p in naked) for l in leaks):
    sys.exit("FAIL: negative control — with no patterns the leaks should ship")
print(f"OK: the emcc exclude patterns prune every planted leak ({which})")
PY

log "OK: package payload checks passed"
