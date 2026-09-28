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

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL=${TOOL:-build/release/xcodebuild}
# Absolute, because a test runs the tool from a directory other than the
# repository -- and a relative path silently fails to execute there, which
# looks like a tool that produces no output rather than one that was not found.
case $TOOL in
/*) ;;
*) TOOL=$ROOT/$TOOL ;;
esac
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

isnt() {
	if [ "$2" != "$3" ]; then
		ok "$1"
	else
		no "$1" "expected anything but [$2]"
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

# setting_dev <devdir> <project> <key> [extra args...] -> value of <key>
#
# -sdk resolves a name inside the developer directory in force.  A test that
# finds an SDK in one developer directory and asks for it by name while
# another is in force is not testing -sdk; it is testing which directory
# happened to be selected.  This pins the directory so the name means
# something.
setting_dev() {
	_d=$1; _p=$2; _k=$3; shift 3
	DEVELOPER_DIR="$_d" "$TOOL" -project "$_p" -showBuildSettings "$@" 2>/dev/null |
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

		# The developer directory this SDK lives in, for the -sdk
		# assertions below.  Derived from the path rather than
		# remembered from the scan, so it stays right if the set of
		# SDKs chosen above changes.
		sdkroot=${sdk%%/Platforms/*}

		# SDKROOT on the command line is the case the ordering fix exists
		# for: it used to be applied before SDKROOT was resolved, and lost.
		is "$tag: SDK_DIR is the SDK SDKROOT names" \
		    "$sdk" "$(setting "$PROJ" SDK_DIR "SDKROOT=$sdk")"
		is "$tag: SDK_NAME is that SDK's own name" \
		    "$want_name" "$(setting "$PROJ" SDK_NAME "SDKROOT=$sdk")"
		is "$tag: SDK_VERSION is that SDK's own version" \
		    "$want_ver" "$(setting "$PROJ" SDK_VERSION "SDKROOT=$sdk")"

		# -sdk has to outrank the SDKROOT the project wrote.  This
		# project hardcodes MacOSX.Internal.sdk, so -sdk naming any
		# SDK at all is enough to see whether the merge put the
		# project's back.  SDK_NAME is expected to be the requested
		# SDK's own CanonicalName, read from its SDKSettings.plist,
		# which pins down *which* SDK answered.
		#
		# Run with DEVELOPER_DIR on the directory this SDK came from,
		# so the name resolves and the assertion is about -sdk.
		#
		# SDK_DIR is only checked for having moved, not for matching
		# the loop's own $sdk: the unversioned entries are the real
		# directories and the versioned ones beside them the
		# symlinks, so asking by CanonicalName lands on a path whose
		# spelling depends on that layout rather than on -sdk.
		is "$tag: -sdk names the SDK it selected" \
		    "$want_name" \
		    "$(setting_dev "$sdkroot" "$PROJ" SDK_NAME -sdk "$want_name")"
		isnt "$tag: -sdk moves SDK_DIR off the project's own" \
		    "$(setting_dev "$sdkroot" "$PROJ" SDK_DIR)" \
		    "$(setting_dev "$sdkroot" "$PROJ" SDK_DIR -sdk "$want_name")"

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
echo "the source root is absolute"

# Every path in the settings is written as $(SRCROOT)/... and expanded against
# it, so leaving SRCROOT as "." or as the project argument verbatim made the
# whole chain of build directories relative -- 31 settings where Apple's are
# absolute, and one that silently loses the leading '/'.  The expected value is
# computed from the project file's own location, not read from Apple's output.
projdir=$(cd "$(dirname "$PROJ")" && pwd -P)

is "SRCROOT is the project's directory, absolute" \
    "$projdir" "$(setting "$PROJ" SRCROOT)"
is "SOURCE_ROOT agrees with it" \
    "$projdir" "$(setting "$PROJ" SOURCE_ROOT)"
is "PROJECT_DIR agrees with it" \
    "$projdir" "$(setting "$PROJ" PROJECT_DIR)"

# The leading slash is the part that is easy to lose, and a relative value
# cannot be caught by a substring test that only looks at the tail.
is "SRCROOT does not start with a bare Users" \
    "" "$(setting "$PROJ" SRCROOT | sed -n 's|^Users/|lost: /Users/|p')"

# A project named by an absolute path must report the same root, or the
# resolution would depend on how the tool happened to be invoked.
absroot=$(setting "$PROJ" SRCROOT)
is "invoking by absolute path gives the same root" \
    "$absroot" "$(setting "$projdir/$(basename "$PROJ")" SRCROOT)"

# This is the case that was actually broken.  Naming the project relatively --
# "xcodebuild -project Foo.xcodeproj", which is how it is normally typed --
# leaves xc_dirname() with a bare name or a short relative path, and taking
# that as the root made every derived setting relative.  Named absolutely the
# old code was already right, so a test that only ever passes an absolute path
# cannot see the defect at all.
relroot=$(cd "$projdir" && "$TOOL" -project "$(basename "$PROJ")" \
    -showBuildSettings 2>/dev/null | sed -n 's/^    SRCROOT = //p' | head -1)
is "naming the project relatively still gives an absolute root" \
    "$absroot" "$relroot"

# And a relative project reached through a ".." path from an unrelated
# directory, where the naive answer is the caller rather than the project.  The
# way back to the root is one ".." per component of the scratch path, which is
# exact and needs no relpath(1) or second interpreter.
depth=$(/usr/bin/printf '%s' "$scratch" | tr -cd '/' | wc -c | tr -d ' ')
ups=$(/usr/bin/printf '../%.0s' $(seq 1 "$depth"))
subroot=$(cd "$scratch" && "$TOOL" \
    -project "$ups$projdir/$(basename "$PROJ")" -showBuildSettings 2>/dev/null |
    sed -n 's/^    SRCROOT = //p' | head -1)
is "a '..' project path resolves to the project, not the caller" \
    "$absroot" "$subroot"

# And from a different working directory, which is where "./build" used to
# resolve -- relative to the caller rather than to the project.  The project is
# named absolutely so that it is the tool's own resolution being tested, not the
# shell's; the comparison is made outside the subshell because a subshell cannot
# add to the pass and fail counts.
othercwd=$(cd / && "$TOOL" -project "$projdir/$(basename "$PROJ")" \
    -showBuildSettings 2>/dev/null | sed -n 's/^    SRCROOT = //p' | head -1)
is "invoking from another directory does not change the root" \
    "$absroot" "$othercwd"

# The chain that hangs off SRCROOT inherits it, so one of them is enough to
# show the expansion followed.
is "a derived build directory is absolute too" \
    "" "$(setting "$PROJ" PROJECT_TEMP_DIR | sed -n 's|^\([A-Za-z]\).*|relative: \1|p')"

echo
echo "the build directories follow the project's own OBJROOT"

# The tool seeded SYMROOT and OBJROOT with the same directory and derived the
# intermediates from that seed, before the project's settings were merged.  This
# project overrides both -- OBJROOT = $(SRCROOT)/build/obj/$(CONFIGURATION) and
# CONFIGURATION_BUILD_DIR = $(SRCROOT)/build/release -- so every derived
# directory pointed somewhere Apple did not.  The seed still has to exist, for
# the merge to expand against; what matters is that the settled values win.
# Each expectation is built from the values the tool reports, which the pbxproj
# itself declares, so no Apple is consulted.
cfg=$(setting "$PROJ" CONFIGURATION)
objroot=$(setting "$PROJ" OBJROOT)
symroot=$(setting "$PROJ" SYMROOT)
cfgbuild=$(setting "$PROJ" CONFIGURATION_BUILD_DIR)
pname=$(setting "$PROJ" PROJECT_NAME)
tname=$(setting "$PROJ" TARGET_NAME)

# The project's own declarations, read from the pbxproj: if these stop matching
# what the tool reports, the project changed and the expectations below are
# what has to be re-read, not silently re-derived.  Note the project writes
# OBJROOT as a literal "build/obj/release" while the configuration is named
# "Release", so the declaration cannot be rebuilt from CONFIGURATION and has to
# be read.
is "CONFIGURATION_BUILD_DIR comes from the project" \
    "$projdir/build/release" "$cfgbuild"

rel_objroot=$(/usr/bin/printf '%s' "$objroot" | sed "s|^$projdir/||")
declared=
for s in $(sed -n 's/^[[:space:]]*OBJROOT = "\$(SRCROOT)\/\([^"]*\)";/\1/p' \
    "$PROJ/project.pbxproj" | sort -u); do
    if [ "$rel_objroot" = "$s" ]; then
        declared=$s
    fi
done
is "OBJROOT is one the project declares" \
    "build/obj/release" "$declared"

# OBJROOT is the base of the whole intermediates chain, and is not the same as
# SYMROOT -- the seed had them equal, which is what put everything one level
# too high.
is "OBJROOT is not SYMROOT" \
    "" "$([ "$objroot" = "$symroot" ] && echo same)"

is "PROJECT_TEMP_DIR hangs off OBJROOT" \
    "$objroot/$pname.build" "$(setting "$PROJ" PROJECT_TEMP_DIR)"
is "CONFIGURATION_TEMP_DIR is the configuration's share" \
    "$objroot/$pname.build/$cfg" "$(setting "$PROJ" CONFIGURATION_TEMP_DIR)"
is "TARGET_TEMP_DIR is the target's share" \
    "$objroot/$pname.build/$cfg/$tname.build" "$(setting "$PROJ" TARGET_TEMP_DIR)"
is "OBJECT_FILE_DIR is under the target's own directory" \
    "$objroot/$pname.build/$cfg/$tname.build/Objects" \
    "$(setting "$PROJ" OBJECT_FILE_DIR)"
is "DERIVED_FILE_DIR is under it too" \
    "$objroot/$pname.build/$cfg/$tname.build/DerivedSources" \
    "$(setting "$PROJ" DERIVED_FILE_DIR)"

# The products follow CONFIGURATION_BUILD_DIR, which the project moved; the
# capitalised "Release" in the seed is what used to leak through here.
is "BUILT_PRODUCTS_DIR is where the project said" \
    "$cfgbuild" "$(setting "$PROJ" BUILT_PRODUCTS_DIR)"
is "TARGET_BUILD_DIR is where the project said" \
    "$cfgbuild" "$(setting "$PROJ" TARGET_BUILD_DIR)"
is "CODESIGNING_FOLDER_PATH follows the products" \
    "$cfgbuild/$(setting "$PROJ" FULL_PRODUCT_NAME)" \
    "$(setting "$PROJ" CODESIGNING_FOLDER_PATH)"

# The strings step is per-architecture.  macOS has no per-architecture strings
# step, so Apple leaves the slot literally undefined.  Observed constant across
# Debug/Release and across ARCHS=arm64, x86_64 and both -- so the expectation is
# the literal word, not a guess about which arch is "current".
is "STRINGSDATA_DIR carries the undefined-architecture slot" \
    "$objroot/$pname.build/$cfg/$tname.build/Objects-normal/undefined_arch" \
    "$(setting "$PROJ" STRINGSDATA_DIR)"

# The built products directory goes on the FRONT of both search paths, which is
# how one target finds a framework another has just built.  The separator lives
# in the prefix, so with nothing declared by the project the value ends in a
# space -- that trailing space is part of Apple's value, not noise.
is "FRAMEWORK_SEARCH_PATHS starts with the products directory" \
    "$cfgbuild " "$(setting "$PROJ" FRAMEWORK_SEARCH_PATHS)"
is "HEADER_SEARCH_PATHS starts with the products include directory" \
    "$cfgbuild/include $projdir/src/common $projdir/src/xcodebuild" \
    "$(setting "$PROJ" HEADER_SEARCH_PATHS)"

# The project declares both source directories and neither products path, so
# the two above are entirely Apple's doing.  Turn the switch off and they must
# come back empty, which is the half that proves the prepend is the switch's
# doing rather than an accident of this project.
nosw="$(setting "$PROJ" HEADER_SEARCH_PATHS ENABLE_DEFAULT_HEADER_SEARCH_PATHS=NO)"
is "with the switch off, HEADER_SEARCH_PATHS is just the project's" \
    "$projdir/src/common $projdir/src/xcodebuild" "$nosw"
noswf="$(setting "$PROJ" FRAMEWORK_SEARCH_PATHS ENABLE_DEFAULT_HEADER_SEARCH_PATHS=NO)"
is "with the switch off, FRAMEWORK_SEARCH_PATHS is empty" \
    "" "$noswf"

# Apple's own CoreBuildSystem.xcspec defines ARCHS_BASE as $(ARCHS), not
# $(ARCHS_STANDARD).  This project narrows ARCHS to $(NATIVE_ARCH_ACTUAL), so
# the two disagree and a hardcoded standard list reports a fat binary's worth of
# architectures for a thin one.
is "ARCHS_BASE follows ARCHS, not the standard list" \
    "$(setting "$PROJ" ARCHS)" "$(setting "$PROJ" ARCHS_BASE)"

# A command-line CONFIGURATION_BUILD_DIR= has to move everything derived from
# it.  "The command line is the last word" means last in the input, not last
# thing to happen: deriving before the overrides land left the products
# directory reporting where it was before, while the key the user typed showed
# the new value -- so the two disagreed.
is "a command-line CONFIGURATION_BUILD_DIR moves the products directory" \
    "/tmp/ovr-prod" \
    "$(setting "$PROJ" BUILT_PRODUCTS_DIR CONFIGURATION_BUILD_DIR=/tmp/ovr-prod)"
is "and moves the search paths built on it" \
    "/tmp/ovr-prod " \
    "$(setting "$PROJ" FRAMEWORK_SEARCH_PATHS CONFIGURATION_BUILD_DIR=/tmp/ovr-prod)"

# TOOLCHAINS is the toolchain's bundle identifier, which its own
# ToolchainInfo.plist records.  The directory is called XcodeDefault.xctoolchain;
# reporting that name is reporting the wrong thing.
#
# Skipped when the resolved toolchain has no ToolchainInfo.plist -- the
# CommandLineTools MacOSX.xctoolchain is an empty directory, and this suite is
# meant to run without an Xcode installed.  The assertion is about reading the
# plist, so with no plist there is nothing to assert.
tcroot="$(setting "$PROJ" TOOLCHAIN_ROOT)"
tcplist="$tcroot/ToolchainInfo.plist"
if [ -f "$tcplist" ]; then
    is "TOOLCHAINS is the toolchain's identifier, not its directory name" \
        "$(/usr/bin/plutil -extract Identifier raw "$tcplist" 2>/dev/null)" \
        "$(setting "$PROJ" TOOLCHAINS)"
    is "TOOLCHAINS is not just the toolchain's name" \
        "" "$([ "$(setting "$PROJ" TOOLCHAINS)" = "$(basename "$tcroot" .xctoolchain)" ] && echo same)"
else
    skipt "TOOLCHAINS is the toolchain's identifier" \
        "$tcroot has no ToolchainInfo.plist"
fi

echo
printf '%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
