#!/bin/sh
# trace_profile.sh <pom2_headless> <out-dir> — UI-017 end to end.
# hello.as runs on the test port with --trace-file, --profile and the cc65
# .dbg's symbols: it must exit 3, its trace must open on the program's own
# first instruction (crt0's LDX #$FF at $0803, the registers the ROM left),
# and the callgrind file must be well formed and name _main and _add.
set -u
bin=$1; out=$2
mkdir -p "$out"
rm -f "$out/hello.trace" "$out/hello.callgrind"
"$bin" --test-port 7 --load tests/testport/hello.as --symbols tests/testport/hello.dbg \
    --trace-file "$out/hello.trace" --profile "$out/hello.callgrind" > "$out/stdout" 2> "$out/log"
status=$?
[ "$status" -eq 3 ] || { cat "$out/log"; echo "trace_profile: exit $status, wanted 3" >&2; exit 1; }
first=$(head -1 "$out/hello.trace")
want="0803 main  A2 FF     LDX #\$FF          A=A0 X=00 Y=01 P=A4 SP=F0  Nv-bdIzc  2"
[ "$first" = "$want" ] || { echo "trace_profile: first trace line"; echo " got: [$first]"; echo "want: [$want]"; exit 1; } >&2
head -1 "$out/hello.callgrind" | grep -qx "# callgrind format" || { echo "trace_profile: no callgrind header" >&2; exit 1; }
grep -qx "events: Cycles Visits" "$out/hello.callgrind" || { echo "trace_profile: no events line" >&2; exit 1; }
for fn in _main _add; do
    grep -qx "fn=$fn" "$out/hello.callgrind" || { echo "trace_profile: no fn=$fn" >&2; exit 1; }
done
echo "trace_profile: trace and callgrind profile OK ($(wc -l < "$out/hello.trace") trace lines)"
