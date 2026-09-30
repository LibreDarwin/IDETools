#!/bin/sh
# Compare -showBuildSettingsForIndex against Apple's xcodebuild.
# The output must match byte for byte once the invocation echo is removed, and
# so must the exit status and the diagnostic.
#
# Two things about Apple's own output are normalised away, and only these two:
#
#   * The invocation echo.  The console form opens by naming the command it
#     ran, which names the binary that was run, so ours and Apple's can never
#     be equal.  It is stripped through the blank line that ends it, which
#     leaves the per-target headers in place to be compared.
#
#   * The result-bundle notice Apple writes to stderr before a refusal, which
#     carries a timestamp, a process id and a path in a temporary directory.
#
#   * Apple's own logging on stderr -- the timestamped, process-id-prefixed
#     "[MT] IDERunDestination:" lines, which some of the fixtures provoke and
#     which say nothing about the answer.  Every diagnostic that answers the
#     request is compared; these are the tool talking to itself.
#
# The filter is applied to both sides, so that it is a difference in the two
# tools' own diagnostics that fails a case rather than a difference in whether
# Xcode felt like logging that morning.
quiet_apple() {
  sed -e '/Writing error result bundle/d' -e '/\[MT\]/d'
}
#
# Environment:
#   MINE     the reimplementation; `make forindex-parity` passes the one it
#            just built
#   XCODE    Apple's xcodebuild, for anyone with Xcode somewhere else
#   CONFIG   the build directory the default MINE is taken from
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
CONFIG=${CONFIG:-release}
MINE=${MINE:-$ROOT/build/$CONFIG/xcodebuild}
ORACLE=${XCODE:-/Applications/Xcode.app/Contents/Developer/usr/bin/xcodebuild}
FIX=$ROOT/tests/fixtures
P=$ROOT/IDETools.xcodeproj

if [ ! -x "$ORACLE" ]; then
  printf 'no Apple xcodebuild at %s -- set XCODE to one; skipped\n' "$ORACLE"
  exit 0
fi

pass=0; fail=0

# strip < file > out
#
# Drops the echo and nothing else.  The echo is a header line, then the command
# indented, then a blank line; -json has no echo at all, so this is a no-op for
# it.  Everything the index service actually answers with is compared, headers
# included.
strip() {
  awk '
    /^Command line invocation:$/ { echo = 1; next }
    echo && /^$/ { echo = 0; next }
    !echo { print }
  '
}

# cmp_case <label> [args...]
#
# stdout and stderr are compared separately rather than merged, because a shell
# gives the two streams different buffering and a merge would then make the
# order they were written in part of what is being measured.
#
# A failing diff is cut short: a single record is several hundred lines, and a
# disagreement about one path would otherwise bury every other one.
cmp_case() {
  label="$1"; shift
  "$ORACLE" "$@" >/tmp/fi_oo 2>/tmp/fi_oe; orc=$?
  "$MINE"   "$@" >/tmp/fi_mo 2>/tmp/fi_me; mrc=$?
  strip </tmp/fi_oo >/tmp/fi_on
  strip </tmp/fi_mo >/tmp/fi_mn
  quiet_apple </tmp/fi_oe >/tmp/fi_oen
  quiet_apple </tmp/fi_me >/tmp/fi_men
  if [ "$orc" = "$mrc" ] && cmp -s /tmp/fi_on /tmp/fi_mn && cmp -s /tmp/fi_oen /tmp/fi_men; then
    pass=$((pass+1)); printf 'PASS  %s\n' "$label"
  else
    fail=$((fail+1)); printf 'FAIL  %s   rc oracle=%s mine=%s\n' "$label" "$orc" "$mrc"
    diff /tmp/fi_on  /tmp/fi_mn  | sed 's/^/    out| /' | head -12
    diff /tmp/fi_oen /tmp/fi_men | sed 's/^/    err| /' | head -12
  fi
}

# --- every project, in both forms ------------------------------------------
#
# Two's files are not on disk, so its records are empty; Coll's two targets
# collide on a scheme; Order lists its targets in the reverse of the order both
# implementations report; Sources points outside the project.  Between them
# they are the interesting shapes.
for p in "$P" "$FIX/Two.xcodeproj" "$FIX/Coll.xcodeproj" "$FIX/Order.xcodeproj" "$FIX/Sources.xcodeproj"; do
  b=$(basename "$p" .xcodeproj)
  [ -d "$p" ] || continue
  cmp_case "$b plain"            -project "$p" -showBuildSettingsForIndex
  cmp_case "$b json"             -project "$p" -showBuildSettingsForIndex -json
  cmp_case "$b json -quiet"      -project "$p" -showBuildSettingsForIndex -json -quiet
  cmp_case "$b -alltargets"      -project "$p" -showBuildSettingsForIndex -alltargets
  cmp_case "$b json -alltargets" -project "$p" -showBuildSettingsForIndex -json -alltargets
  cmp_case "$b quiet"            -project "$p" -showBuildSettingsForIndex -quiet
done

# --- one target at a time --------------------------------------------------
# The library is the only product here that is not a tool, so it is the case
# that says whether the arguments depend on the kind of product.
cmp_case "loader target"      -project "$P" -showBuildSettingsForIndex -target libxcodebuildLoader.dylib
cmp_case "loader json"        -project "$P" -showBuildSettingsForIndex -json -target libxcodebuildLoader.dylib
cmp_case "xcb target"         -project "$P" -showBuildSettingsForIndex -target xcodebuild
cmp_case "xcb json"           -project "$P" -showBuildSettingsForIndex -json -target xcodebuild
cmp_case "default is the tool" -project "$P" -showBuildSettingsForIndex

T=$FIX/Two.xcodeproj
if [ -d "$T" ]; then
  cmp_case "two hello"          -project "$T" -showBuildSettingsForIndex -target hello
  cmp_case "two world"          -project "$T" -showBuildSettingsForIndex -target world
  cmp_case "two world json"     -project "$T" -showBuildSettingsForIndex -json -target world
fi

# --- a target named more than once -----------------------------------------
# Apple's usage calls -target repeatable, and given two of them it answers
# with both targets' settings.  One block for the pair would make the second
# name unreachable.
cmp_case "two -target"            -project "$T" -showBuildSettingsForIndex -target hello -target world
cmp_case "two -target json"       -project "$T" -showBuildSettingsForIndex -json -target hello -target world
cmp_case "two -target reversed"   -project "$T" -showBuildSettingsForIndex -target world -target hello
cmp_case "the same target twice"  -project "$T" -showBuildSettingsForIndex -target hello -target hello
cmp_case "the same target 2x json" -project "$T" -showBuildSettingsForIndex -json -target hello -target hello
cmp_case "three -target"          -project "$T" -showBuildSettingsForIndex -target hello -target world -target hello

# --- a target the project does not have ------------------------------------
# These are the refusals, and they are compared on all three of stdout, status
# and the diagnostic.  Asking for one that exists alongside one that does not
# is the case that says the checking happens first: a record emitted for the
# first before the refusal would be something the index service could act on.
cmp_case "no such target"          -project "$T" -showBuildSettingsForIndex -target nope
cmp_case "no such target json"     -project "$T" -showBuildSettingsForIndex -json -target nope
cmp_case "no such target quiet"    -project "$T" -showBuildSettingsForIndex -target nope -quiet
cmp_case "one real, one not"       -project "$T" -showBuildSettingsForIndex -target hello -target nope
cmp_case "one real, one not json"  -project "$T" -showBuildSettingsForIndex -json -target hello -target nope
cmp_case "first of three not"      -project "$T" -showBuildSettingsForIndex -target nope -target hello -target world
cmp_case "no such target, abs path" -project "$T" -showBuildSettingsForIndex -target nope
# The refusal repeats the project name as the command line gave it, so the form
# of the path is part of what is compared: the same project asked for two ways
# is refused two ways.
cmp_case "no such target, rel path" -project tests/fixtures/Two.xcodeproj -showBuildSettingsForIndex -target nope
cmp_case "no such target, slash"   -project "$T/" -showBuildSettingsForIndex -target nope
cmp_case "no such target, alltargets" -project "$P" -showBuildSettingsForIndex -alltargets -target nope
# An empty name is a name, and there is no target under it.  Answering with a
# default instead would be answering a question that was not asked.
cmp_case "empty target name"           -project "$T" -showBuildSettingsForIndex -target ''
cmp_case "empty target name json"      -project "$T" -showBuildSettingsForIndex -json -target ''
cmp_case "real name, then empty"       -project "$T" -showBuildSettingsForIndex -target hello -target ''

O=$FIX/Order.xcodeproj
if [ -d "$O" ]; then
  cmp_case "order hello"        -project "$O" -showBuildSettingsForIndex -target hello
  cmp_case "order world json"   -project "$O" -showBuildSettingsForIndex -json -target world
fi

# --- arguments the index build does not take -------------------------------
# These are the ones worth pinning to the oracle, because each is a place where
# answering from the command line would look right and be wrong: the index
# service is answered from the project and the build record, so a configuration
# or a setting named here must make no difference to the answer at all.  If
# these ever stop matching, the arguments have started to be honoured.
cmp_case "-configuration ignored"        -project "$P" -showBuildSettingsForIndex -target xcodebuild -configuration Debug
cmp_case "-configuration ignored json"   -project "$P" -showBuildSettingsForIndex -json -target xcodebuild -configuration Release
cmp_case "setting override ignored"      -project "$P" -showBuildSettingsForIndex -target xcodebuild GCC_OPTIMIZATION_LEVEL=0
cmp_case "several overrides ignored"     -project "$P" -showBuildSettingsForIndex -target xcodebuild GCC_OPTIMIZATION_LEVEL=0 OTHER_CFLAGS=-DZZZ ONLY_ACTIVE_ARCH=NO
cmp_case "-sdk ignored"                  -project "$P" -showBuildSettingsForIndex -target xcodebuild -sdk macosx
cmp_case "-sdk ignored json"             -project "$P" -showBuildSettingsForIndex -json -target xcodebuild -sdk macosx
cmp_case "-arch ignored"                 -project "$P" -showBuildSettingsForIndex -target xcodebuild -arch x86_64
cmp_case "-alltargets with a target"     -project "$P" -showBuildSettingsForIndex -alltargets -target xcodebuild
cmp_case "-alltargets with a target json" -project "$P" -showBuildSettingsForIndex -json -alltargets -target xcodebuild

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
