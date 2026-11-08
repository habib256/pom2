#!/usr/bin/env bash
# Same archive and SHA-512 as the GLFW 3.4 port at our vcpkg baseline.
# codeload avoids the github.com/archive endpoint's intermittent 503s.
set -euo pipefail
dest="${VCPKG_DOWNLOADS:-${VCPKG_INSTALLATION_ROOT:?}/downloads}"
if command -v cygpath >/dev/null 2>&1; then
    dest="$(cygpath -u "$dest")"
fi
mkdir -p "$dest"
archive="$dest/glfw-glfw-3.4.tar.gz"
curl -fLsS --connect-timeout 10 --max-time 90 --retry 3 --retry-delay 2 \
    https://codeload.github.com/glfw/glfw/tar.gz/refs/tags/3.4 -o "$archive"
echo "39ad7a4521267fbebc35d2ff0c389a56236ead5fa4bdff33db113bd302f70f5f2869ff4e6db1979512e1542813292dff5a482e94dfce231750f0746c301ae9ed  $archive" | sha512sum -c -
