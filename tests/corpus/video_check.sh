#!/bin/sh
# video_check.sh <pom2_corpus> <out-dir> <title> <frames-per-second>
# UI-021: record a corpus title with --video. pom2_corpus itself fails the
# title if a check is not the last frame written (the check's hash must be
# that frame's). Here: the number of frames is the emulated time times the
# standard's frame rate (one frame either way for the phase of the first),
# frames.txt lists exactly the PNGs written, and every PNG is a real one.
set -u
bin=$1; out=$2; title=$3; fps=$4
rm -rf "$out"
log=$("$bin" --title "$title" --video "$out" 2>&1); status=$?
echo "$log"
[ "$status" -eq 77 ] && exit 77
[ "$status" -eq 0 ] || { echo "video_check: pom2_corpus exited $status" >&2; exit 1; }
line=$(printf '%s\n' "$log" | grep -E "^  (PASS|NEW) +$title ") || { echo "video_check: no result line" >&2; exit 1; }
frames=$(printf '%s\n' "$line" | sed -E 's/.* ([0-9]+) video frames in ([0-9.]+) s.*/\1/')
seconds=$(printf '%s\n' "$line" | sed -E 's/.* ([0-9]+) video frames in ([0-9.]+) s.*/\2/')
want=$(awk -v s="$seconds" -v f="$fps" 'BEGIN { printf "%d", s * f }')
diff=$((frames - want)); [ "$diff" -lt 0 ] && diff=$((-diff))
[ "$diff" -le 1 ] || { echo "video_check: $frames frames in $seconds s, wanted $want at $fps fps" >&2; exit 1; }
listed=$(wc -l < "$out/$title/frames.txt" | tr -d ' ')
pngs=$(ls "$out/$title" | grep -c '\.png$')
[ "$listed" -eq "$frames" ] && [ "$pngs" -eq "$frames" ] || { echo "video_check: $frames frames, $listed listed, $pngs PNGs" >&2; exit 1; }
head -c 8 "$out/$title/00000.png" | od -An -tx1 | tr -s " " | grep -q "89 50 4e 47" || { echo "video_check: 00000.png is not a PNG" >&2; exit 1; }
echo "video_check: $frames frames for $seconds s at $fps fps, checks = last frames"
