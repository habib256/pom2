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
set -euo pipefail
cd "$(dirname "$0")/.."

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
done < <(grep -n 'uses:' "${FILES[@]}" | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

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
done < <(grep -nE "$IMAGE_RE" "${FILES[@]}" | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

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
