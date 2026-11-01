#!/usr/bin/env bash
# check_workflow_pins.sh — every `uses:` and every container image under
# .github/ is pinned to something that cannot move.
#
# A release-day landmine is a dependency that changed under us between two
# tags: an action retagged, a base image rebuilt, a compiler bumped. So:
#   · `uses: owner/repo@REF`  → REF must be a 40-hex commit SHA (a `# vX.Y.Z`
#     comment after it says what it was when pinned);
#   · any line naming a container image → the image must carry `@sha256:`;
#   · `version: latest` is refused anywhere (setup-emsdk's default).
# Local `uses: ./…` references are exempt — but the composite action they point
# at is SCANNED, which is the whole reason the file set below is a find and not
# a glob of workflows/*.yml. Falsifiable: unpin one and it fails.
#
# WHAT THE 2026-09-09 AUDIT FOUND, and why each widening is here. The previous
# version reported "passed" on a tree carrying FOUR unpinned dependencies:
#   1. `container: {image: ghcr.io/owner/b:latest}` — the image regex was
#      anchored `^\s*ghcr\.io/`, so an image that is the VALUE of a key
#      (`image:`, `BUILDER_IMAGE:`) was never looked at. That is the shape
#      release.yml actually uses for its bionic builder.
#   2. `BUILDER_IMAGE: ghcr.io/owner/b:v3` fed to `docker run "$BUILDER_IMAGE"`
#      — same anchor, and the `docker run` line names no image at all.
#   3. `.github/workflows/b.yaml` — the glob was `*.yml`; GitHub accepts both
#      spellings and the `.yaml` half was invisible.
#   4. `.github/actions/*/action.yml` — a composite action's own `uses:` lines
#      were never scanned, so `uses: ./.github/actions/x` laundered anything.
#
# WHAT THE 2026-11-01 HUNT FOUND — a fifth shape of the same class. The image
# regex below tries to RECOGNISE an image, and it only knows a dotted registry
# host (`ghcr.io/…`) or one of six distro names. So every Docker Hub
# short-name image sailed through unpinned while the script said "passed":
#   image: emscripten/emsdk:3.1.64   container: node:20
#   image: myorg/b:latest            image: python:3.12-slim
#   docker run --rm alpine sh …      (no tag at all)
# The realistic trigger here is the WASM leg: making the browser build
# hermetic means a `container: emscripten/emsdk:<tag>`, and two runs of a
# moving tag produce different bundles — the non-reproducibility the pi400
# digest pin exists to prevent. The fix inverts the question: any value of an
# `image:` / `container:` / `*_IMAGE:` key, and any `docker run|pull`
# argument, MUST carry `@sha256:`. Recognising images is left as a second,
# additive pass for images named in other positions.
#
# `--self-test` (this script was the only guard in the pre-build block without
# one, which is why the gap stayed invisible for so long) builds a throwaway
# .github tree and asserts each shape above is refused.
set -euo pipefail

# ── self-test ──────────────────────────────────────────────────────────────
# Negative controls for every shape the guard is supposed to refuse, each in a
# throwaway .github tree. Runs before the real scan in CI, like the other
# guards' self-tests, and needs no network and no build tree.
if [ "${1:-}" = "--self-test" ]; then
    here="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    fails=0
    # The script cds to ITS OWN parent, so a fixture tree has to carry a copy
    # of it — running the repo's copy from elsewhere just rescans the repo and
    # every negative control "passes". (That is exactly what the first draft of
    # this self-test did.)
    mk() {  # mk <name> <workflow body>
        rm -rf "$tmp/t"; mkdir -p "$tmp/t/.github/workflows" "$tmp/t/tools"
        cp "$here" "$tmp/t/tools/check_workflow_pins.sh"
        printf '%s\n' "$2" > "$tmp/t/.github/workflows/w.yml"
    }
    expect() {  # expect <pass|fail> <label> <body>
        mk "$2" "$3"
        if bash "$tmp/t/tools/check_workflow_pins.sh" >/dev/null 2>&1; then got=pass; else got=fail; fi
        if [ "$got" = "$1" ]; then printf '  ok    %s\n' "$2"
        else printf '  FAIL  %s (expected %s, got %s)\n' "$2" "$1" "$got"; fails=$((fails+1)); fi
    }
    SHA=0123456789012345678901234567890123456789
    DIG=sha256:0123456789012345678901234567890123456789012345678901234567890123
    expect pass "a fully pinned workflow passes" \
"jobs:
  a:
    steps:
      - uses: actions/checkout@$SHA # v5.1.0"
    expect fail "a tag-pinned action is refused" \
"jobs:
  a:
    steps:
      - uses: actions/checkout@v5"
    expect fail "a dotted-registry image with a moving tag is refused" \
"jobs:
  a:
    container:
      image: ghcr.io/owner/b:latest"
    # The five shapes the recognise-an-image regex was blind to (2026-11-01).
    expect fail "a Docker Hub short name is refused (emscripten/emsdk)" \
"jobs:
  a:
    container:
      image: emscripten/emsdk:3.1.64"
    expect fail "a single-segment image is refused (node)" \
"jobs:
  a:
    container: node:20"
    expect fail "an unknown org image is refused (myorg/b)" \
"jobs:
  a:
    container:
      image: myorg/pom2-builder:latest"
    expect fail "a tagless docker run argument is refused" \
"jobs:
  a:
    steps:
      - run: docker run --rm alpine sh -c true"
    expect fail "'version: latest' is refused" \
"jobs:
  a:
    steps:
      - uses: ./.github/actions/x
        with:
          version: latest"
    expect pass "a digest-pinned image passes" \
"jobs:
  a:
    container:
      image: emscripten/emsdk:3.1.64@$DIG"
    expect pass "a docker run split over lines, image digest-pinned, passes" \
"jobs:
  a:
    steps:
      - run: |
          docker run --rm \\
            -v \"\$PWD\":/work \\
            debian:bookworm@$DIG \\
            bash /work/x.sh"
    expect pass "an image behind a variable passes (its definition is scanned)" \
"env:
  BUILDER_IMAGE: ghcr.io/owner/b:v3@$DIG
jobs:
  a:
    steps:
      - run: docker run --rm \"\$BUILDER_IMAGE\" true"
    if [ "$fails" -ne 0 ]; then
        echo "check_workflow_pins: self-test FAILED ($fails)"; exit 1
    fi
    echo "check_workflow_pins: self-test passed"
    exit 0
fi

cd "$(dirname "$0")/.."

# -H on every grep below: with a SINGLE file in FILES, `grep -n` prints no
# filename, so `file=${line%%:*}` picks up the line NUMBER and the whole parse
# shifts — the script then reports nothing on a one-workflow tree. Found by
# giving this guard its first self-test (2026-11-01), whose fixtures are
# one-file trees.

# Every workflow AND every composite/local action definition, both spellings.
FILES=()
while IFS= read -r f; do FILES+=("$f"); done < <(
    find .github \( -path '*/workflows/*' -o -name 'action.yml' -o -name 'action.yaml' \) \
         \( -name '*.yml' -o -name '*.yaml' \) -type f | sort)
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "check_workflow_pins: no workflow files found under .github/" >&2
    exit 2
fi
echo "check_workflow_pins: scanning ${#FILES[@]} file(s)"

bad=0

# ── `uses:` ────────────────────────────────────────────────────────────────
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    ref=$(printf '%s' "$text" | sed -n 's/.*uses:[[:space:]]*\([^[:space:]#]*\).*/\1/p')
    case "$ref" in
        ./*|'') continue ;;
    esac
    sha=${ref##*@}
    if ! printf '%s' "$sha" | grep -Eq '^[0-9a-f]{40}$'; then
        echo "UNPINNED  $file:$lineno  $ref"
        bad=1
    fi
done < <(grep -nH 'uses:' "${FILES[@]}" | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

# ── container images ───────────────────────────────────────────────────────
# Matched ANYWHERE on the line, not only at its start: the image is normally
# the value of a key (`image:`, `container:`, `BUILDER_IMAGE:`) or an argument
# to `docker run/pull`. A bare distro name must be followed by `:` to count, so
# `runs-on: ubuntu-latest` and `ubuntu-24.04-arm` are not images.
IMAGE_RE='(([a-z0-9.-]+\.[a-z]{2,}(:[0-9]+)?/)[a-z0-9._/-]+|(^|[^a-z0-9._/-])(debian|ubuntu|alpine|fedora|rockylinux|centos))(:[A-Za-z0-9._-]+)'
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    # `uses:` lines are the action pin above, not an image.
    printf '%s' "$text" | grep -q 'uses:' && continue
    printf '%s' "$text" | grep -q '@sha256:' && continue
    echo "UNPINNED IMAGE  $file:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//')"
    bad=1
done < <(grep -nHE "$IMAGE_RE" "${FILES[@]}" | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

# ── images as the VALUE of a key, and docker run/pull arguments ────────────
# Inverted: do not try to recognise an image, require the digest. An
# `${{ … }}` / `$VAR` indirection is skipped — the line that DEFINES the
# variable is itself one of these keys and is scanned there.
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    value=$(printf '%s' "$text" | sed -E 's/^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*:[[:space:]]*//; s/^["'"'"']//; s/["'"'"']$//')
    # A bare `container:` / `image:` key whose value is a nested block, or an
    # indirection, is not an image name.
    case "$value" in
        ''|'|'*|'>'*|'#'*|'${{'*|'$'*) continue ;;
    esac
    printf '%s' "$value" | grep -q '@sha256:' && continue
    echo "UNPINNED IMAGE  $file:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//')"
    bad=1
done < <(grep -nHE '^[[:space:]]*(image|container|[A-Za-z_]*_?IMAGE):[[:space:]]*\S' "${FILES[@]}" \
         | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

# A `docker run` is almost always split over several lines with trailing
# backslashes, and the image is on one of the later ones — so the LOGICAL line
# is what has to be inspected, not the physical one. awk joins them and keeps
# the first physical line's number for the message.
for f in "${FILES[@]}"; do
    while IFS= read -r line; do
        lineno=${line%%:*}; text=${line#*:}
        printf '%s' "$text" | grep -q '@sha256:' && continue
        printf '%s' "$text" | grep -qE '\$\{\{|\$[A-Za-z_{]' && continue   # indirection
        echo "UNPINNED IMAGE  $f:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//' | cut -c1-90)"
        bad=1
    done < <(awk '
        { line = $0
          sub(/^[ \t]+/, "", line)
          if (buf == "") { start = NR; buf = line } else { buf = buf " " line }
          if (line ~ /\\$/) { sub(/\\$/, "", buf); next }
          if (buf ~ /docker[ \t]+(run|pull)[ \t]/ && buf !~ /^#/) print start ":" buf
          buf = "" }
        END { if (buf ~ /docker[ \t]+(run|pull)[ \t]/ && buf !~ /^#/) print start ":" buf }
    ' "$f")
done

# ── `version: latest` ──────────────────────────────────────────────────────
if grep -nE '^\s*version:\s*latest\b' "${FILES[@]}"; then
    echo "UNPINNED  'version: latest'"
    bad=1
fi

if [ "$bad" -ne 0 ]; then
    echo "check_workflow_pins: FAILED — pin to a commit SHA / an image digest (see the header)."
    exit 1
fi
echo "check_workflow_pins: passed."
