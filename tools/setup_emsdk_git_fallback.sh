#!/usr/bin/env bash
# Fallback when setup-emsdk cannot download github.com's main.zip. Fetch the
# official bootstrap at the SDK's exact release commit, then install the SAME SDK.
set -euo pipefail
version="${1:?SDK version required}"
case "$version" in
    6.0.9) revision=5eb0bde7585670252e8ba05e9d361627bffd08b5 ;;
    *) echo "No pinned Emscripten bootstrap for $version" >&2; exit 1 ;;
esac
dest="${RUNNER_TEMP:?}/pom2-emsdk-fallback"
git init -q "$dest"
git -C "$dest" fetch --depth 1 https://github.com/emscripten-core/emsdk.git "$revision"
git -C "$dest" checkout -q FETCH_HEAD
test "$(git -C "$dest" rev-parse HEAD)" = "$revision"
"$dest/emsdk" install "$version"
"$dest/emsdk" activate "$version"
source "$dest/emsdk_env.sh" >/dev/null
for key in EMSDK EMSDK_NODE EMSDK_PYTHON; do
    if [ -n "${!key:-}" ]; then
        printf '%s=%s\n' "$key" "${!key}" >> "${GITHUB_ENV:?}"
    fi
done
printf '%s\n' "$PATH" | tr ':' '\n' >> "${GITHUB_PATH:?}"
emcc --version
