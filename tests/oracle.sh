#!/bin/sh
#
# Oracle harness for xcindex-test parity measurement.
#
# Drives Apple's real xcindex-test against a fixture directory and prints its
# output with timing normalized, so that captured oracle text can be compared
# byte-for-byte against our own tool later.  The same normalizer is used for
# both sides so a diff between "oracle.txt" and "ours.txt" shows only real
# differences.
#
# Usage:
#   tests/oracle.sh <fixture-dir> [-- <console command> [-arg ..] ..]
#
#   <fixture-dir>      a directory whose *.xcodeproj / *.xcworkspace the tool
#                      is pointed at (pass -project with the fixture path).
#   --                 separates tool options from console commands, exactly
#                      as the real tool's own usage line spells it:
#                      xcindex-test [options] [-- <console command>...]
#
# Environment:
#   XCINDEX_TEST       path to the tool under test (defaults to Apple's)
#   STUDIO             path to Apple's xcindex-test (for oracles) -- this is
#                      the only test file that needs an Xcode install; the
#                      regression suite tests/run.sh deliberately does not.
#
# Output layout (byte-exact, after normalization):
#   each command's exit status, then its normalized stdout, then normalized
#   stderr on a following line, blank-line separated.

set -u

ORACLE_TOOL=${STUDIO:-/Applications/Xcode.app/Contents/Developer/usr/bin/xcindex-test}
TOOL=${XCINDEX_TEST:-}
FIXTURE=${1:-}

if [ -z "$FIXTURE" ]; then
    echo "usage: $0 <fixture-dir> [-- <console command> ..]" >&2
    exit 2
fi
shift

# normalize: strip variable timing from the two lines that carry it.
normalize() {
    sed -E -e 's/ in [0-9.]+([eE][-+]?[0-9]+)? seconds/ in <T> seconds/g'
}

# run <label> <tool> [args...]
run() {
    _label=$1; _tool=$2; shift 2
    _out=$("$_tool" "$@" 2>/tmp/xcix.err)
    _rc=$?
    _err=$(normalize </tmp/xcix.err)
    _out=$(printf '%s\n' "$_out" | normalize)

    printf '%s (exit %d)\n' "$_label" "$_rc"
    printf '  stdout:\n%s\n' "$_out"
    if [ -n "$_err" ]; then
        printf '  stderr:\n%s\n' "$_err"
    fi
    printf '\n'
}

if [ "$#" -eq 0 ]; then
    # A bare fixture gets the full console help captured as the canonical
    # oracle -- the one probe every implementation must reproduce.
    run "help        (console, via --)" "$ORACLE_TOOL" \
        -project "$FIXTURE" -- help
    rm -f /tmp/xcix.err
    exit 0
fi

run "console     ($*)" "$ORACLE_TOOL" -project "$FIXTURE" -- "$@"
rm -f /tmp/xcix.err