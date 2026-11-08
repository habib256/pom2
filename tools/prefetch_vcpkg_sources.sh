#!/usr/bin/env bash
# Same archives and SHA-512 as the ports at vcpkg baseline cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3.
# codeload avoids the github.com/archive endpoint's intermittent 503s.
set -euo pipefail
dest="${VCPKG_DOWNLOADS:-${VCPKG_INSTALLATION_ROOT:?}/downloads}"
if command -v cygpath >/dev/null 2>&1; then
    dest="$(cygpath -u "$dest")"
fi
mkdir -p "$dest"
fetch() {
    local repo=$1 revision=$2 digest=$3
    local archive="$dest/${repo//\//-}-${revision##*/}.tar.gz"
    if [ -f "$archive" ] && echo "$digest  $archive" | sha512sum -c - >/dev/null 2>&1; then
        return
    fi
    curl -fLsS --connect-timeout 10 --max-time 300 --retry 2 --retry-delay 2 \
        "https://codeload.github.com/$repo/tar.gz/$revision" -o "$archive"
    echo "$digest  $archive" | sha512sum -c -
}
fetch glfw/glfw refs/tags/3.4 \
    39ad7a4521267fbebc35d2ff0c389a56236ead5fa4bdff33db113bd302f70f5f2869ff4e6db1979512e1542813292dff5a482e94dfce231750f0746c301ae9ed
fetch KhronosGroup/EGL-Registry 3ae2b7c48690d2ce13cc6db3db02dfc0572be65e \
    c7b09ded4964fa427546bd345a29325105b79079b59642214dc8f04de113f42de2bc4272dbbbd4a801d92afc20297442fdfa12043a0900cf1e2b1cd83f260883
fetch KhronosGroup/OpenGL-Registry 0b449b97cdf1043eef5e1f0e235cbbab6ec10c86 \
    148e1bfe4cc199bcc2c23b22d0b3e4988a29389d7f510ba4a6340672dbb7ab99bb836d2c08587499484df704d51a1adf4f0dc3a30d5ad8977ee0ad339163b17e
