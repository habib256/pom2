#!/usr/bin/env bash
#
# package_core_sdk.sh — the POM2 core SDK as one deliverable: the header, the
# archive, the conversion package and a MANIFEST.json saying what they are. The
# manifest's commit and capabilities come from running the installed archive,
# so they describe the bytes in the tarball and not the build tree's opinion.
#
#   tools/package_core_sdk.sh [--allow-dirty] <build-dir> <out-dir>
#
# <build-dir> is a configured tree; the three libraries are built here.
# A tree with uncommitted changes is refused without --allow-dirty: its commit
# would name sources nobody else can check out.
set -euo pipefail

allow_dirty=0
if [ "${1:-}" = "--allow-dirty" ]; then allow_dirty=1; shift; fi
[ $# -eq 2 ] || { echo "usage: $0 [--allow-dirty] <build-dir> <out-dir>" >&2; exit 2; }
build=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  host=darwin-arm64 ;;
    Linux-x86_64)  host=linux-x86_64 ;;
    *) echo "package_core_sdk: no SDK is published for $(uname -sm)" >&2; exit 1 ;;
esac

cmake --build "$build" --target pom2_core pom2_imageconv pom2_prodosvolume --config Release

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage="$work/stage"
cmake --install "$build" --prefix "$stage" --component pom2_core_sdk --config Release
cmake --install "$build" --prefix "$stage" --component pom2_imageconv_sdk --config Release
find "$stage" -name '*.a' -exec strip -S {} \;

# The installed archive, linked the way a consumer links it, says what it is.
cmake -S "$root/examples/pom2_core_consumer" -B "$work/consumer" \
      -DCMAKE_PREFIX_PATH="$stage" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/consumer" --config Release >/dev/null
info=$("$work/consumer/pom2_core_consumer" --sdk-info)

version=$(printf '%s\n' "$info" | sed -n 's/^version=//p')
commit=$(printf '%s\n' "$info" | sed -n 's/^commit=//p')
case "$commit" in
    unknown)
        echo "package_core_sdk: the archive does not know its commit" >&2; exit 1 ;;
    *-dirty)
        if [ "$allow_dirty" != 1 ]; then
            echo "package_core_sdk: $commit has uncommitted changes (--allow-dirty to package anyway)" >&2
            exit 1
        fi ;;
esac

install -m 644 "$root/docs/SDK.md" "$stage/SDK.md"
INFO="$info" HOST="$host" python3 - "$stage" <<'PY'
import hashlib, json, os, pathlib, sys
stage = pathlib.Path(sys.argv[1])
lines = os.environ["INFO"].splitlines()
values = [line.split("=", 1) for line in lines]
files = {str(p.relative_to(stage)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(stage.rglob("*")) if p.is_file()}
host = os.environ["HOST"]
manifest = {
    "schema": 1,
    "version": next(v for k, v in values if k == "version"),
    "commit": next(v for k, v in values if k == "commit"),
    "host": host,
    "capabilities": [v for k, v in values if k == "capability"],
    "system_dependencies": {
        "darwin-arm64": ["CoreAudio", "AudioToolbox", "AudioUnit", "libslirp", "zlib"],
        "linux-x86_64": ["libdl", "pthread", "libm", "libslirp", "zlib"],
    }[host],
    "roms": "none shipped; see SDK.md § Delivery for what each machine loads",
    "files": files,
}
(stage / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY

name="pom2-core-sdk-v$version-$host"
mv "$stage" "$work/$name"
tar -C "$work" -czf "$out/$name.tar.gz" "$name"
echo "$out/$name.tar.gz ($commit)"
