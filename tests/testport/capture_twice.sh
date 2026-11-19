#!/bin/sh
# capture_twice.sh <pom2_headless> <out-dir>
# AUDIO-013 end to end: tune.bin plays three notes on a Mockingboard and exits
# through the test port. Run it twice with --vgm, --ym and --wav: the program
# must exit 0, the capture must hold its ten AY writes, and the two runs must
# give the same three files, byte for byte (the WAV is mixed offline, timed by
# emulated cycles).
set -u
bin=$1; out=$2
mkdir -p "$out"
for n in 1 2; do
    rm -f "$out/t$n.vgm" "$out/t$n.ym" "$out/t$n.wav"
    "$bin" --mockingboard 4 --test-port 7 --load 0x0300:tests/testport/tune.bin \
        --vgm "$out/t$n.vgm" --ym "$out/t$n.ym" --wav "$out/t$n.wav" 2> "$out/log$n"
    status=$?
    [ "$status" -eq 0 ] || { cat "$out/log$n"; echo "capture_twice: run $n exited $status" >&2; exit 1; }
    grep -q "(VGM, 10 AY writes)" "$out/log$n" \
        || { cat "$out/log$n"; echo "capture_twice: run $n did not capture 10 AY writes" >&2; exit 1; }
done
for ext in vgm ym wav; do
    cmp "$out/t1.$ext" "$out/t2.$ext" || { echo "capture_twice: the two .$ext differ" >&2; exit 1; }
done
echo "capture_twice: VGM, YM and WAV identical across two runs"
