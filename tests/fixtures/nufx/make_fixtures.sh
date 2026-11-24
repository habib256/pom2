#!/bin/sh
# Rebuild the NuFX fixtures of tests/exchange_media_test.cpp. Every byte in
# them is POM2's own: text written here, a seeded pseudo-random block, and a
# ProDOS volume pom2_disk formats. Needs NuLib2 3.1 (Andy McFadden, BSD) as
# NULIB2, and pom2_disk / pom2_prodos from a POM2 build as BUILD.
#   NULIB2=/path/to/nulib2 BUILD=build tests/fixtures/nufx/make_fixtures.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${BUILD:-build}" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
mkdir -p src/SUB
i=0; : > src/HELLO.TXT
while [ $i -lt 400 ]; do echo "POM2 NUFX FIXTURE LINE $i: THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG" >> src/HELLO.TXT; i=$((i+1)); done
python3 -c "import random; r=random.Random(1977); open('src/DATA#062000','wb').write(bytes(r.randrange(256) for _ in range(5000)))"
printf 'A NOTE IN A SUBDIRECTORY\r' > src/SUB/NOTE.TXT
(cd src && "$NULIB2" -a -r -e "$work/files.shk" HELLO.TXT 'DATA#062000' SUB > /dev/null)
(cd src && "$NULIB2" -a -r -e -0 "$work/stored.shk" HELLO.TXT 'DATA#062000' SUB > /dev/null)
"$build/pom2_disk" new vol.po --prodos --name FIXTURE > /dev/null
"$build/pom2_prodos" vol.po put src/HELLO.TXT HELLO.TXT > /dev/null
"$NULIB2" -a -k disk.sdk vol.po > /dev/null
# NuLib2 writes LZW/2 only. An LZW/1 thread is built from the first chunk of
# HELLO.TXT's LZW/2 thread: a first chunk starts from an empty table and
# holds no clear code, so its codes are a valid LZW/1 chunk ([D] NufxLib
# Lzw.c). LZW/1 frames it with the CRC of the chunk padded to 4 KiB, the
# volume and escape bytes, then rleLen (4096: no RLE) and the LZW flag.
python3 - files.shk lzw1_thread.bin <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
rec = 48
attrib = struct.unpack_from('<H', b, rec + 6)[0]
threads = struct.unpack_from('<I', b, rec + 10)[0]
name_len = struct.unpack_from('<H', b, rec + attrib - 2)[0]
heads = rec + attrib + name_len
data = heads + 16 * threads
for t in range(threads):
    cls, fmt, kind, crc, eof, comp = struct.unpack_from('<HHHHII', b, heads + 16 * t)
    if cls == 2 and kind == 0:
        assert fmt == 3, 'HELLO.TXT is expected in LZW/2'
        vol, esc = b[data], b[data + 1]
        rle_len, lzw_len = struct.unpack_from('<HH', b, data + 2)
        assert rle_len == 0x8000 | 4096, 'first chunk expected LZW-only'
        codes = b[data + 6: data + 6 + lzw_len - 4]
        break
    data += comp
text = b''.join(b'POM2 NUFX FIXTURE LINE %d: THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG\n' % i for i in range(400))
def crc16(data, crc=0):
    for x in data:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc
open(sys.argv[2], 'wb').write(struct.pack('<HBB', crc16(text[:4096]), vol, esc) + struct.pack('<HB', 4096, 1) + codes)
PY
cp files.shk stored.shk disk.sdk lzw1_thread.bin "$here/"
