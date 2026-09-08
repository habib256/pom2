#!/usr/bin/env bash
# check_workflow_pins.sh — every `uses:` and every container image in
# .github/workflows/ is pinned to something that cannot move.
#
# A release-day landmine is a dependency that changed under us between two
# tags: an action retagged, a base image rebuilt, a compiler bumped. So:
#   · `uses: owner/repo@REF`  → REF must be a 40-hex commit SHA (a `# vX.Y.Z`
#     comment after it says what it was when pinned);
#   · a `docker run … image:tag` line → the image must carry `@sha256:`;
#   · `version: latest` is refused anywhere (setup-emsdk's default).
# Local `uses: ./…` references are exempt. Falsifiable: unpin one and it fails.
set -euo pipefail
cd "$(dirname "$0")/.."
bad=0
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
done < <(grep -n 'uses:' .github/workflows/*.yml | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    if printf '%s' "$text" | grep -Eq '(debian|ubuntu|alpine|fedora|ghcr\.io/[^ ]+):[A-Za-z0-9._-]+' \
       && ! printf '%s' "$text" | grep -q '@sha256:'; then
        echo "UNPINNED IMAGE  $file:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//')"
        bad=1
    fi
done < <(grep -nE '^\s*(debian|ubuntu|alpine|fedora):|^\s*ghcr\.io/|docker run.*(debian|ubuntu|ghcr\.io)' .github/workflows/*.yml | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')
if grep -nE '^\s*version:\s*latest\b' .github/workflows/*.yml; then
    echo "UNPINNED  'version: latest'"
    bad=1
fi
if [ "$bad" -ne 0 ]; then
    echo "check_workflow_pins: FAILED — pin to a commit SHA / an image digest (see the header)."
    exit 1
fi
echo "check_workflow_pins: passed."
