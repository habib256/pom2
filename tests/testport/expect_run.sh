#!/bin/sh
# expect_run.sh <status> <expected-stdout> <command...>
# Runs the command and passes only when it exits with <status> AND its stdout
# is exactly <expected-stdout> (lines joined with \n, a trailing newline each).
# ctest can match output or invert a failure, not demand one exit status.
want_status=$1; want_out=$2; shift 2
out=$("$@")
status=$?
printf '%s\n' "$out"
if [ "$status" -ne "$want_status" ]; then
    echo "expect_run: exit status $status, wanted $want_status" >&2
    exit 1
fi
if [ "$out" != "$(printf "$want_out")" ]; then
    echo "expect_run: stdout differs from the expected lines" >&2
    exit 1
fi
exit 0
