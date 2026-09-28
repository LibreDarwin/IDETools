#!/bin/sh
#
# Regression tests for IDETools.
#
# Every expectation here is derived from something other than the tool under
# test: a project's own pbxproj, or the SDK's own SDKSettings.plist, or a rule
# read off Apple's output.  Nothing compares against Apple's xcodebuild, so
# these run on a machine with no Xcode installed, and none of them can pass by
# agreeing with a bug the two sides share.
#
# The two cases that motivated this file are the default build configuration
# and the SDK identity derived from SDKROOT; see docs/IDETools.md.

set -u

TOOL=${TOOL:-build/release/xcodebuild}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROJ=$ROOT/IDETools.xcodeproj

pass=0
fail=0
skip=0

ok()   { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no()   { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '         %s\n' "$2"; }
skipt(){ skip=$((skip + 1)); printf '  skip %s (%s)\n' "$1" "$2"; }

# is <name> <expected> <actual>
is() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		no "$1" "expected [$2], got [$3]"
	fi
}

if [ ! -x "$TOOL" ]; then
	echo "test: no such tool: $TOOL" >&2
	echo "test: run 'make' first, or set TOOL=" >&2
	exit 1
fi

# setting <project> <key> [extra args...] -> value of <key>
setting() {
	_p=$1; _k=$2; shift 2
	"$TOOL" -project "$_p" -showBuildSettings "$@" 2>/dev/null |
	    sed -n "s/^    $_k = //p" | head -1
}

# The scratch project is a copy of the real one: hand-written pbxproj fixtures
# are a second thing to get wrong, and a copy is guaranteed to parse.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/idetools-tests.XXXXXX") || exit 1
trap 'rm -rf "$scratch"' EXIT INT TERM

# fixture <name> <defaultConfigurationName or "none">
fixture() {
	_name=$1; _cfg=$2
	rm -rf "$scratch/$_name"
	mkdir -p "$scratch/$_name"
	cp -R "$PROJ" "$scratch/$_name/Project.xcodeproj"
	if [ "$_cfg" = none ]; then
		# blanking the key is what "declares none" has to look like
		sed -i '' 's/^\([[:space:]]*\)defaultConfigurationName = .*;/\1defaultConfigurationName = "";/' \
		    "$scratch/$_name/Project.xcodeproj/project.pbxproj"
	else
		sed -i '' "s/^\([[:space:]]*\)defaultConfigurationName = .*/\1defaultConfigurationName = $_cfg;/" \
		    "$scratch/$_name/Project.xcodeproj/project.pbxproj"
	fi
	echo "$scratch/$_name/Project.xcodeproj"
}

echo "default build configuration"

# The invariant behind the first fix: what the build uses and what the project
# declares must be the same answer.  -list already reported the project's
# defaultConfigurationName while the build silently used Debug, and no setting
# output said so.
default_listed=$(sed -n 's/^[[:space:]]*defaultConfigurationName = \(.*\);/\1/p' "$PROJ/project.pbxproj" | head -1)
is "the build uses the default the project declares" \
    "$default_listed" "$(setting "$PROJ" CONFIGURATION)"

for cfg in Debug Release; do
	f=$(fixture "$cfg" "$cfg")
	is "a project declaring $cfg builds $cfg with no -configuration" \
	    "$cfg" "$(setting "$f" CONFIGURATION)"
	is "a project declaring $cfg is overridden by -configuration Release" \
	    Release "$(setting "$f" CONFIGURATION -configuration Release)"
	is "a project declaring $cfg is overridden by -configuration Debug" \
	    Debug "$(setting "$f" CONFIGURATION -configuration Debug)"
done

f=$(fixture nodefault none)
is "a project declaring no default falls back to Release" \
    Release "$(setting "$f" CONFIGURATION)"

echo
echo "SDK identity from SDKROOT"

# The SDKs to test against: whatever this machine has.  Each one's identity is
# read from its own SDKSettings.plist, so the test never takes our word for it.
sdks=""
for root in "${DEVELOPER_DIR:-}" /Applications/Xcode.app/Contents/Developer \
    /Library/Developer/CommandLineTools; do
	[ -n "$root" ] || continue
	for d in "$root"/Platforms/*/Developer/SDKs/*.sdk; do
		[ -d "$d" ] || continue
		case " $sdks " in *" $d "*) continue ;; esac
		sdks="$sdks $d"
	done
done
set -- $sdks
sdk_count=$#
[ "$sdk_count" -gt 3 ] && set -- "$1" "$2" "$3"

if [ "$sdk_count" -eq 0 ]; then
	skipt "SDK identity follows SDKROOT" "no SDKs found on this machine"
else
	for sdk in "$@"; do
		plist=$sdk/SDKSettings.plist
		if [ ! -f "$plist" ]; then
			skipt "$(basename "$sdk")" "no SDKSettings.plist"
			continue
		fi
		want_name=$(plutil -extract CanonicalName raw -o - "$plist" 2>/dev/null)
		want_ver=$(plutil -extract Version raw -o - "$plist" 2>/dev/null)
		if [ -z "$want_name" ] || [ -z "$want_ver" ]; then
			skipt "$(basename "$sdk")" "plist has no CanonicalName/Version"
			continue
		fi

		tag=$(basename "$sdk")

		# SDKROOT on the command line is the case the ordering fix exists
		# for: it used to be applied before SDKROOT was resolved, and lost.
		is "$tag: SDK_DIR is the SDK SDKROOT names" \
		    "$sdk" "$(setting "$PROJ" SDK_DIR "SDKROOT=$sdk")"
		is "$tag: SDK_NAME is that SDK's own name" \
		    "$want_name" "$(setting "$PROJ" SDK_NAME "SDKROOT=$sdk")"
		is "$tag: SDK_VERSION is that SDK's own version" \
		    "$want_ver" "$(setting "$PROJ" SDK_VERSION "SDKROOT=$sdk")"

		# The numbered forms are derived, so they are checked against the
		# rule rather than copied: each part padded to two and run
		# together, MAJOR padded to four, MINOR cut at the minor part.
		# No SDK on any machine so far has carried a third component, so
		# the patch position is exercised as 0 and the rule is taken from
		# Apple's own encoding of the first two.
		V_MAJOR=$(printf '%s' "$want_ver" | cut -d. -f1)
		V_MINOR=$(printf '%s' "$want_ver" | cut -d. -f2)
		V_PATCH=$(printf '%s' "$want_ver" | cut -d. -f3)
		[ -n "$V_MINOR" ] || V_MINOR=0
		[ -n "$V_PATCH" ] || V_PATCH=0
		want_actual=$(printf '%02d%02d%02d' "$V_MAJOR" "$V_MINOR" "$V_PATCH")
		want_maj=$(printf '%d0000' "$V_MAJOR")
		want_min=$(printf '%02d%02d00' "$V_MAJOR" "$V_MINOR")
		is "$tag: SDK_VERSION_ACTUAL is $want_actual" \
		    "$want_actual" "$(setting "$PROJ" SDK_VERSION_ACTUAL "SDKROOT=$sdk")"
		is "$tag: SDK_VERSION_MAJOR is $want_maj" \
		    "$want_maj" "$(setting "$PROJ" SDK_VERSION_MAJOR "SDKROOT=$sdk")"
		is "$tag: SDK_VERSION_MINOR is $want_min" \
		    "$want_min" "$(setting "$PROJ" SDK_VERSION_MINOR "SDKROOT=$sdk")"
	done
fi

# An SDK in the older layout answers from info.ini instead.  No such SDK
# exists to be pointed at, so one is written: the point is that the sync reads
# an SDK the same way the defaults do, rather than only the modern plist.
old=$scratch/OldLayout.sdk
mkdir -p "$old"
printf '[SDK]\nname = macosx9.9\nversion = 9.9\n' > "$old/info.ini"
is "an SDK that describes itself in info.ini still names itself" \
    macosx9.9 "$(setting "$PROJ" SDK_NAME "SDKROOT=$old")"
is "its info.ini version is read too" \
    9.9 "$(setting "$PROJ" SDK_VERSION "SDKROOT=$old")"
is "and SDK_DIR follows SDKROOT" \
    "$old" "$(setting "$PROJ" SDK_DIR "SDKROOT=$old")"

# The modern plist has to win where both are present, or the fallback would
# quietly override what a real SDK says.
both=$scratch/BothLayouts.sdk
mkdir -p "$both"
printf '[SDK]\nname = stale\nversion = 1.0\n' > "$both/info.ini"
cat > "$both/SDKSettings.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CanonicalName</key><string>current</string>
	<key>Version</key><string>26.5</string>
</dict>
</plist>
PLIST
is "SDKSettings.plist wins over a stale info.ini" \
    current "$(setting "$PROJ" SDK_NAME "SDKROOT=$both")"
is "and the version with it" \
    26.5 "$(setting "$PROJ" SDK_VERSION "SDKROOT=$both")"

# A plist-only SDK with no plist, which is not an SDK, leaves the defaults
# alone.  read_sdk_info() returning success on a missing file would turn a
# typo'd SDKROOT into an empty identity, so it has to report failure.
none=$scratch/NoSuchLayout.sdk
mkdir -p "$none"
is "a directory that describes itself in neither layout names nothing" \
    "" "$(setting "$PROJ" SDK_NAME "SDKROOT=$none")"
is "and does not blank the version either" \
    "" "$(setting "$PROJ" SDK_VERSION "SDKROOT=$none")"

echo
echo "explicit settings override the derived ones"

# An SDK_VERSION= given on the command line must survive the sync, or the
# second pass of the overrides exists for nothing.
is "SDK_VERSION=99.1 wins over the SDK's own version" \
    99.1 "$(setting "$PROJ" SDK_VERSION SDK_VERSION=99.1)"
is "SDK_VERSION_ACTUAL=123 wins over the derived value" \
    123 "$(setting "$PROJ" SDK_VERSION_ACTUAL SDK_VERSION_ACTUAL=123)"

# And an override the sync does not touch must survive it too, which is the
# whole reason the overrides are applied a second time rather than just once.
is "an unrelated override still wins after the sync" \
    custom "$(setting "$PROJ" GCC_OPTIMIZATION_LEVEL GCC_OPTIMIZATION_LEVEL=custom)"

echo
printf '%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
