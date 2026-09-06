#!/usr/bin/env bash
# Regenerate every committed icon raster from packaging/POM2.svg.
#
# The repository ships four icon artefacts and, until this script existed,
# all four were hand-made binaries that could (and did) drift from the SVG:
#
#   packaging/POM2-{16,32,48,64,128,256}.png   hicolor theme (Linux install)
#   packaging/macos/POM2.icns                  CFBundleIconFile of the .app
#   packaging/windows/POM2.ico                 embedded in POM2.exe via POM2.rc.in
#
# Run this after ANY edit to packaging/POM2.svg, then commit the results.
#
# Requirements:
#   rsvg-convert  (brew install librsvg / apt install librsvg2-bin) — the one
#                 renderer we trust. ImageMagick's built-in MSVG renderer
#                 silently drops `fill="url(#gradient)"`, which would flatten
#                 the badge and the apple to black.
#   python3       — runs tools/png2ico.py, which assembles the .ico.
#                 `magick *.png out.ico` is NOT used: it stores every entry
#                 as an uncompressed DIB (25 KB -> 370 KB, all of it landing
#                 in POM2.exe via POM2.rc.in).
#   iconutil      (macOS only) — assembles the .icns. Elsewhere that one
#                 artefact is skipped and left as committed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
svg="$here/packaging/POM2.svg"
[ -f "$svg" ] || { echo "gen_icons: missing $svg" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "gen_icons: '$1' not found — $2" >&2; exit 1; }
}
need rsvg-convert "brew install librsvg (macOS) / apt install librsvg2-bin"
need python3      "install Python 3"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# At 16 physical pixels the "][" is 0.8 px of bar and renders as bright mush
# that swallows the apple — measured against bar thicknesses 11, 14 and 17,
# none of which survive (see the note in POM2.svg). That one slot therefore
# drops the bracket group and lets the apple carry the icon. The simplified
# art is DERIVED from the master right here rather than kept as a second
# file, so the two cannot drift apart.
plain="$tmp/POM2-nobrackets.svg"
python3 - "$svg" "$plain" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8").read()
mark = '  <g fill="#f4f8ff">'
i = s.index(mark)
j = s.index('</g>', i) + len('</g>\n')
open(dst, "w", encoding="utf-8").write(s[:i] + s[j:])
PY

render() {   # render <size> <out>
    local src="$svg"
    [ "$1" -le 16 ] && src="$plain"
    rsvg-convert -w "$1" -h "$1" "$src" -o "$2"
}

# ── hicolor PNGs ────────────────────────────────────────────────────────────
# Sizes are the ones CMakeLists.txt installs into
# share/icons/hicolor/<N>x<N>/apps; keep the two lists in step.
for sz in 16 32 48 64 128 256; do
    render "$sz" "$here/packaging/POM2-$sz.png"
    echo "  packaging/POM2-$sz.png"
done

# ── Windows .ico ────────────────────────────────────────────────────────────
# Same size ladder as the PNGs (POM2.rc.in documents 16..256). Every entry is
# rendered from the vector rather than downscaled from one big raster, so
# each size gets its own clean hinting-free rasterisation.
ico_inputs=()
for sz in 16 32 48 64 128 256; do
    render "$sz" "$tmp/ico-$sz.png"
    ico_inputs+=("$tmp/ico-$sz.png")
done
python3 "$here/tools/png2ico.py" "$here/packaging/windows/POM2.ico" "${ico_inputs[@]}"
echo "  packaging/windows/POM2.ico"

# ── macOS .icns ─────────────────────────────────────────────────────────────
# The full modern iconset, @2x included — the previous .icns stopped at 256
# and carried non-standard 48/64 slots, so Retina Finder and the Dock had to
# upscale. iconutil only accepts these exact names.
if command -v iconutil >/dev/null 2>&1; then
    set="$tmp/POM2.iconset"
    mkdir -p "$set"
    render 16   "$set/icon_16x16.png"
    render 32   "$set/icon_16x16@2x.png"
    render 32   "$set/icon_32x32.png"
    render 64   "$set/icon_32x32@2x.png"
    render 128  "$set/icon_128x128.png"
    render 256  "$set/icon_128x128@2x.png"
    render 256  "$set/icon_256x256.png"
    render 512  "$set/icon_256x256@2x.png"
    render 512  "$set/icon_512x512.png"
    render 1024 "$set/icon_512x512@2x.png"
    iconutil -c icns "$set" -o "$here/packaging/macos/POM2.icns"
    echo "  packaging/macos/POM2.icns"
else
    echo "  packaging/macos/POM2.icns SKIPPED (iconutil is macOS-only)" >&2
fi

echo "gen_icons: done — commit the regenerated files."
