#!/bin/sh
# Compare the reimplementation against Apple's xcindex-test.
# Timings are normalised away; everything else must match byte for byte.
#
# Environment:
#   MINE     the reimplementation; `make parity` passes the one it just built
#   STUDIO   Apple's xcindex-test, for anyone with Xcode somewhere else
#   CONFIG   the build directory the default MINE is taken from
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=${CONFIG:-release}
MINE=${MINE:-$ROOT/build/$CONFIG/xcindex-test}
ORACLE=${STUDIO:-/Applications/Xcode.app/Contents/Developer/usr/bin/xcindex-test}
FIX=$ROOT/tests/fixtures
T=$FIX/Two.xcodeproj

pass=0; fail=0

norm() { sed -E 's/in [-+0-9.e]+ seconds/in T seconds/'; }

# cmp_case <label> <stdin> [args...]
cmp_case() {
  label="$1"; stdin="$2"; shift 2
  printf '%s' "$stdin" | "$ORACLE" "$@" >/tmp/pc_oo 2>/tmp/pc_oe; orc=$?
  printf '%s' "$stdin" | "$MINE"   "$@" >/tmp/pc_mo 2>/tmp/pc_me; mrc=$?
  norm </tmp/pc_oo >/tmp/pc_on; norm </tmp/pc_mo >/tmp/pc_mn
  norm </tmp/pc_oe >/tmp/pc_oen; norm </tmp/pc_me >/tmp/pc_men
  if [ "$orc" = "$mrc" ] && cmp -s /tmp/pc_on /tmp/pc_mn && cmp -s /tmp/pc_oen /tmp/pc_men; then
    pass=$((pass+1)); printf 'PASS  %s\n' "$label"
  else
    fail=$((fail+1)); printf 'FAIL  %s   rc oracle=%s mine=%s\n' "$label" "$orc" "$mrc"
    diff /tmp/pc_on  /tmp/pc_mn  | sed 's/^/    out| /'
    diff /tmp/pc_oen /tmp/pc_men | sed 's/^/    err| /'
  fi
}

cmp_case "empty stdin"          ""  -project $T
cmp_case "list-schemes"         ""  -project $T -- list-schemes
cmp_case "spaces-only line"     "   
"  -project $T
cmp_case "tab-only line"        "	
"  -project $T
cmp_case "list-indexables"      ""  -project $T -- list-indexables
cmp_case "print-stats"          ""  -project $T -- print-stats
cmp_case "list-indexables -target hello"  ""  -project $T -- list-indexables -target hello
cmp_case "list-indexables -target world"  ""  -project $T -- list-indexables -target world
cmp_case "skip 1 of 2"          ""  -project $T -- list-indexables -all-targets -skip-first-n 1
cmp_case "skip 0"               ""  -project $T -- list-indexables -target hello -skip-first-n 0
cmp_case "print-stats -target hello"  ""  -project $T -- print-stats -target hello
cmp_case "beep"                 ""  -project $T -- beep
cmp_case "REPL list-schemes"    "list-schemes
"  -project $T
cmp_case "REPL blank then cmd"  "
list-schemes
"  -project $T
cmp_case "REPL quit"            "quit
"  -project $T
cmp_case "unknown action"       "frobnicate
"  -project $T
cmp_case "CLI unknown action"   ""  -project $T -- frobnicate
cmp_case "bad target"           ""  -project $T -- list-indexables -target nope
cmp_case "bad scheme"           ""  -project $T -- list-schemes -scheme nope
cmp_case "qos bad"              ""  -project $T -- list-indexables -target hello -qos bogus
cmp_case "j bad"                ""  -project $T -- list-indexables -target hello -j abc
cmp_case "j -1"                 ""  -project $T -- list-indexables -target hello -j -1
cmp_case "j 0"                  ""  -project $T -- list-indexables -target hello -j 0
cmp_case "unknown arg"          ""  -project $T -- list-indexables -zzz
cmp_case "no project"           ""  -- list-schemes
cmp_case "missing project file" ""  -project /tmp/DoesNotExist.xcodeproj -- list-schemes
cmp_case "two actions"          ""  -project $T -- list-schemes beep
cmp_case "targets-of-scheme"    ""  -project $T -- list-indexables -targets-of-scheme MyHello
cmp_case "targets-not-of"       ""  -project $T -- list-indexables -targets-not-of-scheme MyHello
cmp_case "dup targets"          ""  -project $T -- list-indexables -target hello -target hello
cmp_case "first error aborts"   ""  -project $T -- list-schemes -- list-indexables -target nope -- list-schemes
cmp_case "list-schemes -target bad" ""  -project $T -- list-schemes -target nope

# --- tool options and console argument validation --------------------------
#
# A command line with more than one unrecognised "-..." token is left out:
# Apple reports one of them out of an unordered collection, so which one
# appears varies between runs.  Every case here has exactly one.
cmp_case "tool unknown argument"  ""  -project $T -bogus x -- list-schemes
cmp_case "tool unknown input"     ""  -project $T bogus -- list-schemes
cmp_case "tool -option=value"     ""  -project $T -derivedDataPath=/tmp/dd -- list-schemes
cmp_case "tool need path wins"    ""  -bogus
cmp_case "tool arg beats input"   ""  -project $T bare -aaa -- list-schemes
cmp_case "tool -project no value" ""  -project -bogus -- list-schemes
cmp_case "tool dangling -project" ""  -project
cmp_case "tool empty -project"    ""  -project "" -- list-schemes
cmp_case "tool -project twice"    ""  -project $T -project $T -- list-schemes
cmp_case "tool -help beats all"   ""  -project $T -help -bogus5
cmp_case "tool flags accepted"    ""  -project $T -debugActivityLog -continue-after-errors -derivedDataPath /tmp/dd -- list-schemes
cmp_case "tool non-project path"  ""  -project /tmp/NotThere.txt -- list-schemes
cmp_case "tool directory"         ""  -project "$FIX" -- list-schemes
cmp_case "console no action"      "-zzz
"  -project $T
cmp_case "console qos no action"  "-qos bogus
"  -project $T
cmp_case "console two actions"    "list-schemes print-stats
"  -project $T
cmp_case "console -- ends line"   "list-schemes -- print-stats
"  -project $T
cmp_case "console -- alone"       "list-schemes --
"  -project $T
cmp_case "console two then --"     "list-schemes print-stats --
"  -project $T
cmp_case "console qos beats two"  "list-schemes print-stats -qos bogus
"  -project $T
cmp_case "console j beats two"    "list-schemes print-stats -j abc
"  -project $T
cmp_case "console two beats arg"  "list-schemes print-stats -zzz
"  -project $T
cmp_case "console no action beats arg" "-zzz
"  -project $T
cmp_case "console qos beats no action" "-qos bogus
"  -project $T
cmp_case "REPL -- then cmd"       "help -- print-stats
"  -project $T

# --- help text -----------------------------------------------------------
cmp_case "tool -help"           ""  -help
cmp_case "console help"         ""  -project $T -- help
cmp_case "console help REPL"    "help
"  -project $T
cmp_case "help with bad scheme" ""  -project $T -- help -scheme nope
cmp_case "list-schemes -help"   ""  -project $T -- list-schemes -help

# --- scheme selectors, every scheme --------------------------------------
for s in MyHello Both world; do
  cmp_case "of-scheme $s"        ""  -project $T -- list-indexables -targets-of-scheme $s
  cmp_case "not-of-scheme $s"    ""  -project $T -- list-indexables -targets-not-of-scheme $s
  cmp_case "stats of-scheme $s"  ""  -project $T -- print-stats -targets-of-scheme $s
done
cmp_case "of-scheme bad"       ""  -project $T -- list-indexables -targets-of-scheme nope
cmp_case "not-of-scheme bad"   ""  -project $T -- list-indexables -targets-not-of-scheme nope
cmp_case "list-schemes bad-of" ""  -project $T -- list-schemes -targets-of-scheme nope

# --- all-targets and skip interactions ------------------------------------
cmp_case "all-targets"           ""  -project $T -- list-indexables -all-targets
cmp_case "all + bad target"      ""  -project $T -- list-indexables -all-targets -target nope
cmp_case "all + bad of-scheme"   ""  -project $T -- list-indexables -all-targets -targets-of-scheme nope
cmp_case "all + bad not-of"      ""  -project $T -- list-indexables -all-targets -targets-not-of-scheme nope
cmp_case "all + bad -scheme"     ""  -project $T -- list-indexables -all-targets -scheme nope
cmp_case "skip 2 of 2"            ""  -project $T -- list-indexables -all-targets -skip-first-n 2
cmp_case "skip 99"               ""  -project $T -- list-indexables -all-targets -skip-first-n 99
cmp_case "skip on default"       ""  -project $T -- list-indexables -skip-first-n 1
cmp_case "skip on list-schemes"  ""  -project $T -- list-schemes -skip-first-n 1
cmp_case "skip on stats"         ""  -project $T -- print-stats -all-targets -skip-first-n 1
cmp_case "skip REPL"             "list-indexables -target hello -skip-first-n 1
"  -project $T
cmp_case "skip missing value"    ""  -project $T -- list-indexables -all-targets -skip-first-n
cmp_case "skip negative"         ""  -project $T -- list-indexables -all-targets -skip-first-n -1

# --- validation precedence ------------------------------------------------
cmp_case "qos beats target"  ""  -project $T -- list-indexables -target nope -qos bogus
cmp_case "qos beats scheme"  ""  -project $T -- list-schemes -scheme nope -qos bogus
cmp_case "qos beats unknown" ""  -project $T -- list-indexables -zzz -qos bogus
cmp_case "qos beats j"       ""  -project $T -- list-indexables -j abc -qos bogus
cmp_case "j beats unknown"   ""  -project $T -- list-indexables -zzz -j abc
cmp_case "unknown beats tgt" ""  -project $T -- list-indexables -target nope -zzz
cmp_case "scheme beats tgt"  ""  -project $T -- list-indexables -target nope -scheme nope
cmp_case "j 4"               ""  -project $T -- list-indexables -target hello -j 4
cmp_case "skip abc"          ""  -project $T -- list-indexables -all-targets -skip-first-n abc

# --- option value edge cases ---------------------------------------------
cmp_case "target= form"      ""  -project $T -- list-indexables -target=hello
cmp_case "scheme= form"      ""  -project $T -- list-indexables -scheme=MyHello
cmp_case "target then dash"  ""  -project $T -- list-indexables -target -zzz
cmp_case "empty target"      ""  -project $T -- list-indexables -target
cmp_case "quiet"             ""  -project $T -- list-schemes -quiet
cmp_case "v flag"            ""  -project $T -- list-schemes -v
cmp_case "unknown flag -q"   ""  -project $T -- list-schemes -q
cmp_case "bare dash"         ""  -project $T -- list-schemes -

# --- the second fixture ---------------------------------------------------
S=$FIX/Sources.xcodeproj
if [ -d "$S" ]; then
  cmp_case "fixture list-schemes"    ""  -project $S -- list-schemes
  cmp_case "fixture list-indexables" ""  -project $S -- list-indexables
  cmp_case "fixture print-stats"     ""  -project $S -- print-stats
  cmp_case "fixture target"          ""  -project $S -- list-indexables -target hello
  cmp_case "fixture scheme"          ""  -project $S -- list-indexables -scheme hello
  cmp_case "fixture stats scheme"    ""  -project $S -- print-stats -scheme hello
  cmp_case "fixture beep"            ""  -project $S -- beep
fi

# --- target order is the name order, not the file order -------------------
# The Order fixture lists its targets array as world, hello; both Apple
# and we report hello, world.  An unsorted implementation would emit the
# file order here and fail.
O=$FIX/Order.xcodeproj
if [ -d "$O" ]; then
  cmp_case "order list-indexables"   ""  -project $O -- list-indexables
  cmp_case "order all-targets"       ""  -project $O -- list-indexables -all-targets
  cmp_case "order of-scheme"         ""  -project $O -- list-indexables -targets-of-scheme Both
  cmp_case "order not-of-scheme"     ""  -project $O -- list-indexables -targets-not-of-scheme Both
  cmp_case "order skip 1"            ""  -project $O -- list-indexables -all-targets -skip-first-n 1
fi

# --- a scheme file that only builds does not suppress a target scheme -----
# Coll's scheme file is named world and only builds hello, so the derived
# hello and world schemes both survive and world is listed twice.
C=$FIX/Coll.xcodeproj
if [ -d "$C" ]; then
  cmp_case "collision list-schemes" ""  -project $C -- list-schemes
  cmp_case "collision list-indexables" ""  -project $C -- list-indexables
fi

# --- no skipping line when the arguments do not get that far --------------
cmp_case "skip suppressed by target" ""  -project $T -- list-indexables -target nope -skip-first-n 1
cmp_case "skip suppressed by qos"    ""  -project $T -- list-indexables -target hello -skip-first-n 1 -qos bogus

# --- the console ahead of the engine --------------------------------------
# These actions need XCBuild to do their work, so the work is out of scope.
# What is in scope, and checked here, is everything the console settles first:
# that a target set is demanded where Apple demands one, that -target and the
# two scheme selectors resolve before any per-action prerequisite, and the
# missing-prerequisite errors themselves.  The cases that would go on to build
# are deliberately absent, because matching them would mean matching the
# engine.
cmp_case "prepare needs a target set"    ""  -project $T -- prepare
cmp_case "index-files needs a target set" ""  -project $T -- index-files
cmp_case "settings needs a target set"   ""  -project $T -- print-index-build-settings
cmp_case "a bare scheme is not a set"    ""  -project $T -- prepare -scheme MyHello
cmp_case "settings missing description"  ""  -project $T -- print-index-build-settings -all-targets
cmp_case "print-build-description"       ""  -project $T -- print-build-description
cmp_case "print-build-description-tgts"  ""  -project $T -- print-build-description-targets
cmp_case "print-destination"             ""  -project $T -- print-destination
cmp_case "destination ignores target"    ""  -project $T -- print-destination -target nope
cmp_case "scheme ignores target"         ""  -project $T -- list-schemes -target nope
cmp_case "beep ignores target"           ""  -project $T -- beep -target nope
cmp_case "prepare resolves target"       ""  -project $T -- prepare -target nope
cmp_case "index-files resolves target"   ""  -project $T -- index-files -target nope
cmp_case "settings resolves target"      ""  -project $T -- print-index-build-settings -target nope
cmp_case "prepare resolves of-scheme"    ""  -project $T -- prepare -targets-of-scheme nope
cmp_case "prepare resolves not-scheme"   ""  -project $T -- prepare -targets-not-of-scheme nope
cmp_case "build-desc resolves target"    ""  -project $T -- create-build-description -target nope
cmp_case "build-desc wants destination"  ""  -project $T -- create-build-description -all-targets
cmp_case "build-desc of-scheme dest"     ""  -project $T -- create-build-description -targets-of-scheme MyHello
cmp_case "build-desc skips then dest"    ""  -project $T -- create-build-description -all-targets -skip-first-n 1

# --- project path forms ---------------------------------------------------
cmp_case "pbxproj path"     ""  -project $T/project.pbxproj -- list-indexables
cmp_case "trailing slash"   ""  -project $T/ -- list-schemes

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
