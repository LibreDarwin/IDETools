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
echo "the developer directory's furniture, under Apple's other two prefixes"

# Apple names one directory three ways: DEVELOPER_*, and then
# SYSTEM_DEVELOPER_* and PLATFORM_DEVELOPER_*.  All three hang off the
# developer directory in force, so a project asking for one gets the tools
# that are building it.
#
# The expected values are computed from the scratch directory below rather than
# read off Apple's output, which is the point: the directory is a plain argument
# here, so a hardcoded Apple path cannot pass.
devroot="$scratch/FakeDeveloper"
mkdir -p "$devroot"

is "SYSTEM_DEVELOPER_DIR is the developer directory itself" \
    "$devroot" "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_DIR)"
is "PLATFORM_DEVELOPER_SDK_DIR hangs off it" \
    "$devroot/Platforms/MacOSX.platform/Developer/SDKs" \
    "$(setting_dev "$devroot" "$PROJ" PLATFORM_DEVELOPER_SDK_DIR)"
is "SYSTEM_DEVELOPER_BIN_DIR follows the developer directory" \
    "$devroot/usr/bin" \
    "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_BIN_DIR)"

# The deeply nested ones are where a single missing path segment shows up, and
# the directory name has a space in it, which is also the parsing risk.
is "the nested documentation paths follow it" \
    "$devroot/ADC Reference Library/documentation/DeveloperTools" \
    "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_TOOLS_DOC_DIR)"
is "and the built-examples path, which has two of them" \
    "$devroot/Applications/Utilities/Built Examples" \
    "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_DEMOS_DIR)"

# The spellings do not follow from each other: APPS_DIR and APPLICATIONS_DIR
# are both /Applications, and SYSTEM_DEVELOPER_TOOLS carries no _DIR at all.  A
# rule derived from the key name gets the first two wrong.
is "APPS_DIR is the applications directory" \
    "$devroot/Applications" \
    "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_APPS_DIR)"
# Pinned absolutely before it is compared to anything: two absent keys agree
# with each other perfectly, so the equality below says nothing on its own.
is "PLATFORM_DEVELOPER_APPLICATIONS_DIR is that same directory" \
    "$devroot/Applications" \
    "$(setting_dev "$devroot" "$PROJ" PLATFORM_DEVELOPER_APPLICATIONS_DIR)"
is "and the two spellings agree" \
    "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_APPS_DIR)" \
    "$(setting_dev "$devroot" "$PROJ" PLATFORM_DEVELOPER_APPLICATIONS_DIR)"
is "SYSTEM_DEVELOPER_TOOLS needs no _DIR to be found" \
    "$devroot/Tools" \
    "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_TOOLS)"

# Two developer directories, two answers.  Without this the assertions above
# would also pass for a table of Apple's own paths, which is the mistake this
# whole family invites.
otherroot="$scratch/OtherDeveloper"
mkdir -p "$otherroot"
is "a different developer directory gives a different answer" \
    "$otherroot/usr/bin" \
    "$(setting_dev "$otherroot" "$PROJ" SYSTEM_DEVELOPER_BIN_DIR)"
is "and the two do not collide" \
    "" "$([ "$(setting_dev "$devroot" "$PROJ" SYSTEM_DEVELOPER_BIN_DIR)" = \
         "$(setting_dev "$otherroot" "$PROJ" SYSTEM_DEVELOPER_BIN_DIR)" ] && echo same)"

# Stripping three components off the tool's own path lands on a directory for
# anything three levels deep, and this harness lives at build/release, so the
# repository root qualifies.  With no configuration to answer instead, that
# root was reported as DEVELOPER_DIR and every developer key was derived
# inside it -- thirty-odd paths with nothing behind them, which is worse than
# the CommandLineTools default it displaced.
#
# An empty HOME stands in for "unconfigured" without touching the real one.
nohome="$scratch/no-home"
mkdir -p "$nohome"
reported=$(env -u DEVELOPER_DIR HOME="$nohome" "$TOOL" -project "$PROJ" \
    -showBuildSettings 2>/dev/null | sed -n 's/^    DEVELOPER_DIR = //p' | head -1)
isnt "an unconfigured run does not adopt the tool's own directory" \
    "$ROOT" "$reported"
# Stated as a property rather than as the compiled-in default, so this keeps
# testing the derivation instead of one machine's answer to it.
is "the reported developer directory has a usr/bin in it" \
    "" "$([ -d "$reported/usr/bin" ] || echo missing)"

# The positive case, without naming a machine-specific staged tree: a copy of
# the tool three levels down inside a synthetic Developer layout must still be
# found.  The staged self-build this repository is built with is exactly that
# shape, and a check that rejected it would break the build it protects.
stage="$scratch/StagedDeveloper"
mkdir -p "$stage/usr/bin"
cp "$TOOL" "$stage/usr/bin/xcodebuild"
# Compared physically: the answer comes back through realpath, and a scratch
# directory under /var/folders is a symlink to /private/var/folders.
stage_physical=$(cd "$stage" && pwd -P)
is "a real developer layout is still recognised" \
    "$stage_physical" \
    "$(env -u DEVELOPER_DIR HOME="$nohome" "$stage/usr/bin/xcodebuild" \
        -project "$PROJ" -showBuildSettings 2>/dev/null |
        sed -n 's/^    DEVELOPER_DIR = //p' | head -1)"

# Seven directories off OBJROOT and CONFIGURATION_BUILD_DIR.  Derived rather
# than seeded, so the first assertion below is the one that matters: the tails
# have to follow an OBJROOT= override, or a caller who redirects the build gets
# settings pointing into a tree it is not building.  Pinned absolutely first --
# two absent keys agree with each other perfectly.
objroot_abs=$("$TOOL" -project "$PROJ" -showBuildSettings OBJROOT="$scratch/ob" 2>/dev/null |
    sed -n 's/^    OBJROOT = //p' | head -1)
is "OBJROOT override is in force" "$scratch/ob" "$objroot_abs"
for tail in CompositeSDKs GeneratedModuleMaps SharedPrecompiledHeaders \
            TemporaryTaskSandboxes UninstalledProducts; do
  case $tail in
    CompositeSDKs) key=COMPOSITE_SDK_DIRS ;;
    GeneratedModuleMaps) key=GENERATED_MODULEMAP_DIR ;;
    SharedPrecompiledHeaders) key=SHARED_PRECOMPS_DIR ;;
    TemporaryTaskSandboxes) key=TEMP_SANDBOX_DIR ;;
    UninstalledProducts) key=UNINSTALLED_PRODUCTS_DIR ;;
  esac
  is "$key follows the OBJROOT override" \
      "$scratch/ob/$tail" "$(setting "$PROJ" "$key" OBJROOT="$scratch/ob")"
done
# None of the five names imply their location, which is why the tails are
# written out above rather than derived from the keys.
is "SHARED_PRECOMPS_DIR is not under SHARED_anything" \
    "$scratch/ob/SharedPrecompiledHeaders" \
    "$(setting "$PROJ" SHARED_PRECOMPS_DIR OBJROOT="$scratch/ob")"
is "and two of the five differ only in plural" \
    "$scratch/ob/GeneratedModuleMaps" \
    "$(setting "$PROJ" GENERATED_MODULEMAP_DIR OBJROOT="$scratch/ob")"

cbd_abs=$("$TOOL" -project "$PROJ" -showBuildSettings CONFIGURATION_BUILD_DIR="$scratch/cb" 2>/dev/null |
    sed -n 's/^    CONFIGURATION_BUILD_DIR = //p' | head -1)
is "SHARED_DERIVED_FILE_DIR follows the products directory" \
    "$scratch/cb/DerivedSources" \
    "$(setting "$PROJ" SHARED_DERIVED_FILE_DIR CONFIGURATION_BUILD_DIR="$scratch/cb")"
# The trailing slash is what Apple emits.  It reads as a typo and normalising
# it away is a mismatch, so the assertion is on the exact string.
is "METAL_LIBRARY_OUTPUT_DIR keeps its trailing slash" \
    "$scratch/cb/" \
    "$(setting "$PROJ" METAL_LIBRARY_OUTPUT_DIR CONFIGURATION_BUILD_DIR="$scratch/cb")"
# Not platform-gated: Apple emits it for iOS and tvOS too, so a macosx gate
# would suppress the key on those platforms rather than reproduce it.
is "and it is emitted for a non-macOS platform as well" \
    "$scratch/cb/" \
    "$(setting "$PROJ" METAL_LIBRARY_OUTPUT_DIR CONFIGURATION_BUILD_DIR="$scratch/cb" -sdk iphoneos)"

# Eight fixed install locations and two home-relative ones.  Pinned absolutely,
# since eight constants that all happen to agree with each other prove nothing.
is "SYSTEM_APPS_DIR is /Applications" "/Applications" "$(setting "$PROJ" SYSTEM_APPS_DIR)"
is "LOCAL_DEVELOPER_DIR is /Library/Developer" \
    "/Library/Developer" "$(setting "$PROJ" LOCAL_DEVELOPER_DIR)"
is "SYSTEM_DOCUMENTATION_DIR is /Library/Documentation" \
    "/Library/Documentation" "$(setting "$PROJ" SYSTEM_DOCUMENTATION_DIR)"
is "SYSTEM_DEMOS_DIR is /Applications/Extras" \
    "/Applications/Extras" "$(setting "$PROJ" SYSTEM_DEMOS_DIR)"

# The prefixes are not a hierarchy, which is why the table is spelled out.  All
# three apps-directory keys collide, so a rule mapping LOCAL_ onto SYSTEM_ by
# dropping or adding a prefix cannot tell them apart.  Pinned absolutely, since
# "they agree" also holds when the keys are simply absent.
is "SYSTEM_APPS_DIR and LOCAL_APPS_DIR are both /Applications" \
    "/Applications /Applications" \
    "$(setting "$PROJ" SYSTEM_APPS_DIR) $(setting "$PROJ" LOCAL_APPS_DIR)"
is "both _ADMIN_APPS_DIR spellings are /Applications/Utilities" \
    "/Applications/Utilities" "$(setting "$PROJ" SYSTEM_ADMIN_APPS_DIR)"
is "and the local one agrees" \
    "/Applications/Utilities" "$(setting "$PROJ" LOCAL_ADMIN_APPS_DIR)"

# But the two _LIBRARY_DIR keys are not the same directory, and the pins above
# are what make this meaningful: with both keys absent they trivially "differ".
is "SYSTEM_LIBRARY_DIR is /System/Library" \
    "/System/Library" "$(setting "$PROJ" SYSTEM_LIBRARY_DIR)"
is "LOCAL_LIBRARY_DIR is /Library, not /System/Library" \
    "/Library" "$(setting "$PROJ" LOCAL_LIBRARY_DIR)"
is "the two _LIBRARY_DIR keys are therefore different directories" \
    "" "$([ "$(setting "$PROJ" LOCAL_LIBRARY_DIR)" = \
            "$(setting "$PROJ" SYSTEM_LIBRARY_DIR)" ] && echo same)"

# The two USER_ keys hang off the passwd home, not off $HOME.  Apple ignores
# $HOME for these: run it with HOME pointing at an empty directory and it still
# reports the real home.  HOME is overridden here so that reading the
# environment would fail, and the expected value is the tool's own HOME setting
# -- which is getpwuid's and was already matching Apple -- so the two can only
# agree if the override did not reach them.
fakehome="$scratch/fake-home"
mkdir -p "$fakehome"
realhome=$(HOME="$fakehome" "$TOOL" -project "$PROJ" -showBuildSettings 2>/dev/null |
    sed -n 's/^    HOME = //p' | head -1)
isnt "and the override really was visible to the environment" "$fakehome" "$realhome"
is "USER_APPS_DIR is under the passwd home" \
    "$realhome/Applications" "$(HOME="$fakehome" setting "$PROJ" USER_APPS_DIR)"
is "USER_LIBRARY_DIR is under the passwd home" \
    "$realhome/Library" "$(HOME="$fakehome" setting "$PROJ" USER_LIBRARY_DIR)"

echo
printf '%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
