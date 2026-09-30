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

# plist_array <plist> <key> -> the CFArray value of <key>, its entries
# joined with spaces.  plutil has no array join, and this is what the tool's
# own suggested-values join answers with, so the oracle is the plist itself.
plist_array() {
	/usr/bin/plutil -convert xml1 -o - "$1" 2>/dev/null |
	    sed -n "/<key>$2<\/key>/,/<\/array>/p" |
	    sed -n 's/.*<string>\([^<]*\)<\/string>.*/\1/p' |
	    paste -sd' ' -
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

		# What the platform says about itself, read from the same plist
		# and checked with -sdk so the platform pass, not just the
		# identity sync, is the thing under test.  Normally one platform
		# is exercised per machine (the two iOS-family SDKs are the same
		# directory shape, and macosx is the default), so these assert
		# the derivation against the plist rather than quoted values --
		# a table keyed off nothing would not know MACOSX_DEPLOYMENT_TARGET
		# from IPHONEOS_DEPLOYMENT_TARGET.
		plat=$(/usr/bin/plutil -extract DefaultProperties.PLATFORM_NAME raw \
		    -o - "$plist" 2>/dev/null)
		if [ -n "$plat" ]; then
			is "$tag: PLATFORM_NAME is the platform's own name" \
			    "$plat" \
			    "$(setting_dev "$sdkroot" "$PROJ" PLATFORM_NAME -sdk "$want_name")"

			dtname=$(/usr/bin/plutil \
			    -extract "SupportedTargets.$plat.DeploymentTargetSettingName" \
			    raw -o - "$plist" 2>/dev/null)
			sys=$(/usr/bin/plutil \
			    -extract "SupportedTargets.$plat.LLVMTargetTripleSys" \
			    raw -o - "$plist" 2>/dev/null)

			is "$tag: the deployment-target setting is the plist's" \
			    "$dtname" \
			    "$(setting_dev "$sdkroot" "$PROJ" DEPLOYMENT_TARGET_SETTING_NAME -sdk "$want_name")"
			is "$tag: the triple sys is the plist's" \
			    "$sys" \
			    "$(setting_dev "$sdkroot" "$PROJ" SWIFT_PLATFORM_TARGET_PREFIX -sdk "$want_name")"

			# The OS version half of the triple is the sys plus the
			# deployment target in force; at report time the $(...)
			# is expanded, so the plist's default target is the oracle
			# and the two halves read apart yet agree.
			is "$tag: the LLVM OS version is the sys plus the default target" \
			    "$sys$(/usr/bin/plutil -extract "DefaultProperties.$dtname" raw -o - "$plist" 2>/dev/null)" \
			    "$(setting_dev "$sdkroot" "$PROJ" LLVM_TARGET_TRIPLE_OS_VERSION -sdk "$want_name")"

			# The default deployment target is under the setting the
			# plist named, so that key has to move with the SDK too.
			is "$tag: the deployment-target default is the plist's" \
			    "$(/usr/bin/plutil -extract "DefaultProperties.$dtname" raw -o - "$plist" 2>/dev/null)" \
			    "$(setting_dev "$sdkroot" "$PROJ" "$dtname" -sdk "$want_name")"

			# The suggested-values list is an array in the plist, and
			# the tool reports it joined with spaces.
			is "$tag: the suggested values are the plist's array, joined" \
			    "$(plist_array "$plist" DEPLOYMENT_TARGET_SUGGESTED_VALUES)" \
			    "$(setting_dev "$sdkroot" "$PROJ" DEPLOYMENT_TARGET_SUGGESTED_VALUES -sdk "$want_name")"

			# Entitlements go in the binary on simulators and in a
			# copy of the profile elsewhere; the boundary is the
			# platform name, so a simulator SDK asserts the other half.
			case $plat in
			*simulator*) want_ent=__entitlements ;;
			*) want_ent=Signature ;;
			esac
			is "$tag: entitlements land in $want_ent" \
			    "$want_ent" \
			    "$(setting_dev "$sdkroot" "$PROJ" ENTITLEMENTS_DESTINATION -sdk "$want_name")"
		else
			skipt "$tag: platform" "plist has no DefaultProperties.PLATFORM_NAME"
		fi
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
echo "the per-user caches, and the digest in one of their names"

# Both cache families are rooted at the Darwin user cache directory, which is
# not $HOME and is reported by confstr(3).  getconf(1) reads the same place, so
# it is the oracle here -- with its trailing slash stripped, because the value
# the tool reports does not carry one and pasting getconf's in would compare two
# different spellings of the same path.
usercache=$(getconf DARWIN_USER_CACHE_DIR)
usercache=${usercache%/}

# This one is not under the Xcode version directory, so it needs no bundle at
# all, which makes it the one cache key that is available everywhere.
is "the clang session file is under the llvm module cache" \
    "$usercache/org.llvm.clang/ModuleCache.noindex/Session.modulevalidation" \
    "$(setting "$PROJ" CLANG_MODULES_BUILD_SESSION_FILE)"

# The versioned caches name the bundle they came from, so they are only right if
# the version is read out of the developer directory in force.  A fake bundle
# carrying a version that cannot occur in a real install is the strong form of
# that test: a hardcoded Apple path, a different key of the same plist, or a
# leftover from the bundle that happens to be selected all miss by construction.
fakebundle="$scratch/FakeXcode.app"
fakedev="$fakebundle/Contents/Developer"
mkdir -p "$fakedev"
/usr/bin/plutil -create xml1 "$fakebundle/Contents/version.plist"
/usr/bin/plutil -insert CFBundleShortVersionString -string 9.9 \
    "$fakebundle/Contents/version.plist"
/usr/bin/plutil -insert ProductBuildVersion -string 999 \
    "$fakebundle/Contents/version.plist"
# The plist also has the key a person would reach for first, and it is not the
# one Apple uses: the two differ here, so a read of the wrong one cannot pass.
# shellcheck disable=SC2016
/usr/bin/plutil -insert CFBundleVersion -string 424242 \
    "$fakebundle/Contents/version.plist"

fakecache="$usercache/com.apple.DeveloperTools/9.9-999/FakeXcode"
is "CACHE_ROOT is the bundle's version and product name" \
    "$fakecache" "$(setting_dev "$fakedev" "$PROJ" CACHE_ROOT)"
is "CCHROOT is the same directory" \
    "$fakecache" "$(setting_dev "$fakedev" "$PROJ" CCHROOT)"
is "SDK_STAT_CACHE_DIR is that directory too" \
    "$fakecache" "$(setting_dev "$fakedev" "$PROJ" SDK_STAT_CACHE_DIR)"
is "and the compilation cache hangs off it" \
    "$fakecache/CompilationCache.noindex" \
    "$(setting_dev "$fakedev" "$PROJ" COMPILATION_CACHE_CAS_PATH)"
is "the product name comes from the bundle, not the developer directory" \
    "" "$([ "$(setting_dev "$fakedev" "$PROJ" CACHE_ROOT)" = \
            "$usercache/com.apple.DeveloperTools/9.9-999/Developer" ] && echo same)"
is "the clang path is the same under any bundle" \
    "$usercache/org.llvm.clang/ModuleCache.noindex/Session.modulevalidation" \
    "$(setting_dev "$fakedev" "$PROJ" CLANG_MODULES_BUILD_SESSION_FILE)"

# A developer directory that is not inside a bundle is the CommandLineTools
# shape.  The versioned caches need a version, and inventing one would name a
# directory Apple never uses, so they are absent instead of guessed at.
is "no bundle means no CACHE_ROOT" \
    "" "$(setting_dev "$devroot" "$PROJ" CACHE_ROOT)"
is "and no CCHROOT" \
    "" "$(setting_dev "$devroot" "$PROJ" CCHROOT)"
is "and no SDK_STAT_CACHE_DIR" \
    "" "$(setting_dev "$devroot" "$PROJ" SDK_STAT_CACHE_DIR)"
is "though the clang session file is still reported" \
    "$usercache/org.llvm.clang/ModuleCache.noindex/Session.modulevalidation" \
    "$(setting_dev "$devroot" "$PROJ" CLANG_MODULES_BUILD_SESSION_FILE)"

# The real bundle, cross-checked against the same plist read with plutil.  The
# expected string is computed from the bundle in force rather than pasted from
# Apple's output, so a stale hardcoded version fails on a different Xcode
# instead of quietly agreeing.
sysdev=$(setting "$PROJ" SYSTEM_DEVELOPER_DIR)
case "$sysdev" in
*/Contents/Developer)
    bundle=${sysdev%/Contents/Developer}
    vplist="$bundle/Contents/version.plist"
    shortv=$(/usr/bin/plutil -extract CFBundleShortVersionString raw "$vplist")
    buildv=$(/usr/bin/plutil -extract ProductBuildVersion raw "$vplist")
    bundlev=$(/usr/bin/plutil -extract CFBundleVersion raw "$vplist")
    is "CACHE_ROOT is the bundle's own version and product name" \
        "$usercache/com.apple.DeveloperTools/$shortv-$buildv/$(basename "$bundle" .app)" \
        "$(setting "$PROJ" CACHE_ROOT)"
    isnt "and the build is not CFBundleVersion" \
        "$usercache/com.apple.DeveloperTools/$shortv-$bundlev/$(basename "$bundle" .app)" \
        "$(setting "$PROJ" CACHE_ROOT)"
    is "the compilation cache is inside it" \
        "$(setting "$PROJ" CACHE_ROOT)/CompilationCache.noindex" \
        "$(setting "$PROJ" COMPILATION_CACHE_CAS_PATH)"
    ;;
*)
    skipt "CACHE_ROOT names the bundle version and product" \
        "$sysdev is not inside a bundle"
    ;;
esac

# The caches are settings, so an override on the command line still wins.  They
# are set as defaults rather than forced, and this is where that shows.
is "an overridden CACHE_ROOT wins over the derived one" \
    /tmp/ovr-cache "$(setting "$PROJ" CACHE_ROOT CACHE_ROOT=/tmp/ovr-cache)"
is "and the derived one is back when the override is gone" \
    "$(setting "$PROJ" CACHE_ROOT)" \
    "$(setting "$PROJ" CACHE_ROOT CACHE_ROOT=$(setting "$PROJ" CACHE_ROOT))"

# The last component of the SDK stat cache name is a digest of the SDK path, so
# it has to be a real digest of the resolved SDK: /sbin/md5 is the oracle, and
# the SDK's own name and build supply the other two fields, read here from the
# SDK's plists rather than from the tool.  -sdk is named explicitly so the test
# exercises a real SDK instead of whichever one happens to be the default.
sdkdir=$(setting "$PROJ" SDK_DIR -sdk macosx)
statpath=$(setting "$PROJ" SDK_STAT_CACHE_PATH -sdk macosx)
if [ -n "$sdkdir" ] && [ -n "$statpath" ]; then
    sdkname=$(setting "$PROJ" SDK_NAME -sdk macosx)
    sdkbuild=$(/usr/bin/plutil -extract ProductBuildVersion raw \
        "$sdkdir/System/Library/CoreServices/SystemVersion.plist")
    digest=$(/usr/bin/printf '%s' "$sdkdir" | /sbin/md5 -q)
    is "the SDK stat cache is named for the SDK in force, by its md5" \
        "$sdkname-$sdkbuild-$digest.sdkstatcache" "$(basename "$statpath")"
    is "and it lives under SDK_STAT_CACHE_DIR, not CACHE_ROOT" \
        "$(setting "$PROJ" SDK_STAT_CACHE_DIR -sdk macosx)/SDKStatCaches.noindex/$(basename "$statpath")" \
        "$statpath"
    isnt "the digest is not of the cache directory instead" \
        "$(setting "$PROJ" SDK_STAT_CACHE_DIR -sdk macosx)/SDKStatCaches.noindex/$sdkname-$sdkbuild-$(printf '%s' "$(setting "$PROJ" SDK_STAT_CACHE_DIR -sdk macosx)" | /sbin/md5 -q).sdkstatcache" \
        "$statpath"
    # The name has to follow -sdk, or the file is shared by every SDK and the
    # cache is wrong the first time two are in play.
    is "CACHE_ROOT does not move with -sdk" \
        "$(setting "$PROJ" CACHE_ROOT -sdk macosx)" \
        "$(setting "$PROJ" CACHE_ROOT -sdk iphoneos)"
    is "but the SDK stat cache file name does" \
        "1" "$([ "$(setting "$PROJ" SDK_STAT_CACHE_PATH -sdk macosx)" != \
            "$(setting "$PROJ" SDK_STAT_CACHE_PATH -sdk iphoneos)" ] && echo 1)"
else
    skipt "the SDK stat cache name carries the SDK's md5" \
        "no macOS SDK stat cache path in this developer directory"
fi

# The -<platform> suffix on a configuration's directory name.  This is the
# thing the document used to record as not reproducible, on the grounds that
# PLATFORM_NAME does not track -sdk.  The name is not only in PLATFORM_NAME, so
# the tests below pin the rule rather than the macosx case only: a suffix that
# is always empty looks identical to a suffix that is always right.
ctd_macosx=$(setting "$PROJ" CONFIGURATION_TEMP_DIR -sdk macosx)
is "macOS carries no platform suffix" \
    "$(setting "$PROJ" OBJROOT -sdk macosx)/IDETools.build/Release" \
    "$ctd_macosx"

# The suffix only exists on a platform that is not the default one, so the
# assertions that mean anything need a non-default SDK to resolve.  CommandLineTools
# has no iOS or watchOS platform at all, and an absent SDK would leave the suffix
# empty on both sides of the comparison and pass without testing anything.
iosroot=$(setting "$PROJ" SDKROOT -sdk iphoneos)
case $iosroot in
*/iPhoneOS*)
    ob_ios=$(setting "$PROJ" OBJROOT -sdk iphoneos)
    is "iphoneos takes the suffix" \
        "$ob_ios/IDETools.build/Release-iphoneos" \
        "$(setting "$PROJ" CONFIGURATION_TEMP_DIR -sdk iphoneos)"
    is "watchos does, and it is the platform and not a version" \
        "$(setting "$PROJ" OBJROOT -sdk watchos)/IDETools.build/Release-watchos" \
        "$(setting "$PROJ" CONFIGURATION_TEMP_DIR -sdk watchos)"
    # A versioned SDK name and a path to the SDK both resolve to the same
    # platform, so neither leaks into the suffix.  These are the two spellings
    # where an implementation that trims digits off the name goes wrong.
    is "a versioned SDK name does not put a version in the suffix" \
        "$ob_ios/IDETools.build/Release-iphoneos" \
        "$(setting "$PROJ" CONFIGURATION_TEMP_DIR \
            -sdk "$(printf '%s' "$iosroot" | sed 's|.*/||; s|[.]sdk$||')")"
    is "a path to the SDK does not put a path in the suffix" \
        "$ob_ios/IDETools.build/Release-iphoneos" \
        "$(setting "$PROJ" CONFIGURATION_TEMP_DIR -sdk "$iosroot")"
    # The one other key that takes the suffix, and the reason it was documented
    # as stuck: four of the five OBJROOT tails are unsuffixed on every platform.
    is "GENERATED_MODULEMAP_DIR takes the same suffix" \
        "$ob_ios/GeneratedModuleMaps-iphoneos" \
        "$(setting "$PROJ" GENERATED_MODULEMAP_DIR -sdk iphoneos)"
    is "and none of the other four do" \
        "$ob_ios/CompositeSDKs" \
        "$(setting "$PROJ" COMPOSITE_SDK_DIRS -sdk iphoneos)"
    ;;
*)
    skipt "the -<platform> suffix on a non-default platform" \
        "no iOS platform in this developer directory"
    ;;
esac

# Sixteen keys under the target's own directory.  Pinned against an
# OBJROOT= override, because a hardcoded path would pass whether or not the
# derivation follows the build location.
troot="$scratch/tb/IDETools.build"
for k in FILE_LIST PKGINFO_FILE_PATH PRECOMP_DESTINATION_DIR REZ_COLLECTOR_DIR; do
  case $k in
    FILE_LIST) tail=Objects/LinkFileList ;;
    PKGINFO_FILE_PATH) tail=PkgInfo ;;
    PRECOMP_DESTINATION_DIR) tail=PrefixHeaders ;;
    REZ_COLLECTOR_DIR) tail=ResourceManagerResources ;;
  esac
  is "$k hangs off the target directory" "$troot/Release/xcodebuild.build/$tail" \
      "$(setting "$PROJ" "$k" OBJROOT="$scratch/tb" -sdk macosx)"
done
is "REZ_OBJECTS_DIR is the collector directory plus Objects" \
    "$troot/Release/xcodebuild.build/ResourceManagerResources/Objects" \
    "$(setting "$PROJ" REZ_OBJECTS_DIR OBJROOT="$scratch/tb" -sdk macosx)"
# These two carry the target's name, so a key-name-derived path gets them
# backwards -- the file is named after the product, not after the directory.
is "DUMP_DEPENDENCIES_OUTPUT_PATH carries the target's name" \
    "$troot/Release/xcodebuild.build/xcodebuild-BuildDependencyInfo.json" \
    "$(setting "$PROJ" DUMP_DEPENDENCIES_OUTPUT_PATH OBJROOT="$scratch/tb" -sdk macosx)"
is "LD_MAP_FILE_PATH does too" \
    "$troot/Release/xcodebuild.build/xcodebuild-LinkMap-normal-undefined_arch.txt" \
    "$(setting "$PROJ" LD_MAP_FILE_PATH OBJROOT="$scratch/tb" -sdk macosx)"
# CURRENT_ARCH, not the arch: this is undefined_arch whatever ARCHS says,
# because -showBuildSettings is being asked for settings and not for a build.
is "PER_ARCH_OBJECT_FILE_DIR uses CURRENT_ARCH, not ARCHS" \
    "$troot/Release/xcodebuild.build/Objects-normal/undefined_arch" \
    "$(setting "$PROJ" PER_ARCH_OBJECT_FILE_DIR OBJROOT="$scratch/tb" ARCHS=arm64 -sdk macosx)"
is "and PER_ARCH_MODULE_FILE_DIR is the same directory" \
    "$(setting "$PROJ" PER_ARCH_OBJECT_FILE_DIR -sdk macosx)" \
    "$(setting "$PROJ" PER_ARCH_MODULE_FILE_DIR -sdk macosx)"
is "PER_VARIANT_OBJECT_FILE_DIR is its parent" \
    "$troot/Release/xcodebuild.build/Objects-normal" \
    "$(setting "$PROJ" PER_VARIANT_OBJECT_FILE_DIR OBJROOT="$scratch/tb" -sdk macosx)"
# The two groups above disagree about what an architecture is -- undefined_arch
# where the per-arch files say arm64 -- so this is the assertion that keeps them
# from being merged into one.
is "the per-arch files use the arch from ARCHS instead" \
    "$troot/Release/xcodebuild.build/Objects-normal/arm64/xcodebuild.LinkFileList" \
    "$(setting "$PROJ" LINK_FILE_LIST_normal_arm64 OBJROOT="$scratch/tb" -sdk macosx)"

# One key per arch in ARCHS, subscripted into the key name.  A single arch is
# the only case the ordinary project exercises, so the multi-arch and empty
# cases are what distinguish an implementation from a hardcoded arm64.
multi=$("$TOOL" -project "$PROJ" -showBuildSettings ARCHS="arm64 x86_64" 2>/dev/null |
    grep -c '^    LINK_FILE_LIST_normal_')
is "two arches give two LINK_FILE_LIST keys" "2" "$multi"
is "and one SWIFT_RESPONSE_FILE_PATH key each" "2" \
    "$("$TOOL" -project "$PROJ" -showBuildSettings ARCHS="arm64 x86_64" 2>/dev/null |
        grep -c '^    SWIFT_RESPONSE_FILE_PATH_normal_')"
is "x86_64 is spelled as itself, not as the first arch" \
    "$troot/Release/xcodebuild.build/Objects-normal/x86_64/xcodebuild.SwiftFileList" \
    "$(setting "$PROJ" SWIFT_RESPONSE_FILE_PATH_normal_x86_64 OBJROOT="$scratch/tb" ARCHS=x86_64 -sdk macosx)"
is "an empty ARCHS gives none of them" "" \
    "$(setting "$PROJ" LINK_FILE_LIST_normal_arm64 ARCHS= -sdk macosx)"

# One word apart, and not the same directory: this one hangs off the project's
# build directory, so it has neither the configuration nor the target in it.
is "PROJECT_DERIVED_FILE_DIR is the project's, not the target's" \
    "$scratch/tb/IDETools.build/DerivedSources" \
    "$(setting "$PROJ" PROJECT_DERIVED_FILE_DIR OBJROOT="$scratch/tb" -sdk macosx)"
is "and it is not the one DERIVED_FILE_DIR names" "1" \
    "$([ "$(setting "$PROJ" PROJECT_DERIVED_FILE_DIR OBJROOT="$scratch/tb" -sdk macosx)" != \
        "$(setting "$PROJ" DERIVED_FILE_DIR OBJROOT="$scratch/tb" -sdk macosx)" ] && echo 1)"
# The whole family follows the platform suffix, which is the reason the base
# was fixed before these keys.  On a platform where the suffix is wrong these
# are all wrong too, and they are plausible-looking paths.
is "and it does not take the platform suffix" \
    "$scratch/tb/IDETools.build/DerivedSources" \
    "$(setting "$PROJ" PROJECT_DERIVED_FILE_DIR OBJROOT="$scratch/tb" -sdk macosx)"
case $iosroot in
*/iPhoneOS*)
    is "while the target's own directories do" \
        "$troot/Release-iphoneos/xcodebuild.build/PkgInfo" \
        "$(setting "$PROJ" PKGINFO_FILE_PATH OBJROOT="$scratch/tb" -sdk iphoneos)"
    ;;
esac

# What this project and target are called.  Nine keys, all read off the
# project's filename and the target's productType, and all of them
# target-specific, so they are pinned for both of this project's targets: a tool
# and a dynamic library, which between them cover both product types this
# project uses, and the difference between a target that sets a version and one
# that does not.
tgt_tool=xcodebuild
tgt_lib=libxcodebuildLoader.dylib

is "PROJECT is the project's own name" IDETools \
    "$(setting "$PROJ" PROJECT -sdk macosx)"
is "TARGETNAME is the target's, and follows -target" "$tgt_lib" \
    "$(setting "$PROJ" TARGETNAME -sdk macosx -target "$tgt_lib")"

# The GUID is the MD5 of the project's *filename*, extension included -- not of
# its contents and not of its directory.  Pinned as a literal, because an
# implementation that hashed the path or the pbxproj would produce a
# well-formed 32 characters and no error.
is "PROJECT_GUID is the digest of the .xcodeproj filename" \
    06c6d787d2b9f8e5eca60b744348fec5 \
    "$(setting "$PROJ" PROJECT_GUID -sdk macosx)"
is "and it is the same for every target, being the project's" \
    06c6d787d2b9f8e5eca60b744348fec5 \
    "$(setting "$PROJ" PROJECT_GUID -sdk macosx -target "$tgt_lib")"

# The symlink is the whole reason the name is canonicalized before it is
# digested.  Taken from the path as typed this reports the alias's digest,
# which is a plausible-looking GUID and is wrong.  Both sides are compared to
# the literal rather than to each other, so a tool that reported nothing at all
# fails here instead of matching an equally empty other side.
alias="$scratch/Alias.xcodeproj"
ln -sfn "$PROJ" "$alias"
is "a symlink to the project still reports the real name's GUID" \
    06c6d787d2b9f8e5eca60b744348fec5 \
    "$(setting "$alias" PROJECT_GUID -sdk macosx)"

# The two productType-dependent keys, for both product types in this project.
is "a tool is a mach-o executable" com.apple.package-type.mach-o-executable \
    "$(setting "$PROJ" PACKAGE_TYPE -sdk macosx -target "$tgt_tool")"
is "stripped with everything" all \
    "$(setting "$PROJ" STRIP_STYLE -sdk macosx -target "$tgt_tool")"
is "a dynamic library is a mach-o dylib" com.apple.package-type.mach-o-dylib \
    "$(setting "$PROJ" PACKAGE_TYPE -sdk macosx -target "$tgt_lib")"
is "and stripped in debugging" debugging \
    "$(setting "$PROJ" STRIP_STYLE -sdk macosx -target "$tgt_lib")"

# Named after the product, which is not the target's name: this target's
# PRODUCT_NAME is xcodebuildLoader while TARGETNAME is the .dylib, so a path
# built from TARGETNAME gets it wrong in a way that looks fine on the tool
# target and wrong only here.
is "VERSION_INFO_FILE follows PRODUCT_NAME, not TARGETNAME" \
    xcodebuildLoader_vers.c \
    "$(setting "$PROJ" VERSION_INFO_FILE -sdk macosx -target "$tgt_lib")"
is "and PRODUCT_NAME is indeed the other name" xcodebuildLoader \
    "$(setting "$PROJ" PRODUCT_NAME -sdk macosx -target "$tgt_lib")"

# The login name from the password database, which is not $USER: with USER set
# to something else and with it unset, Apple still reports the account name.
is "VERSION_INFO_BUILDER is the account name" "$(/usr/bin/id -un)" \
    "$(setting "$PROJ" VERSION_INFO_BUILDER -sdk macosx)"
is "and it is not \$USER" "$(/usr/bin/id -un)" \
    "$(USER=not-the-user setting "$PROJ" VERSION_INFO_BUILDER -sdk macosx)"

# Two spaces after PROGRAM:, and a version-less form that still carries its
# hyphen.  The tool sets no CURRENT_PROJECT_VERSION; the library sets 0.1.0.
is "VERSION_INFO_STRING quotes the whole banner" \
    '"@(#)PROGRAM:xcodebuild  PROJECT:IDETools-"' \
    "$(setting "$PROJ" VERSION_INFO_STRING -sdk macosx -target "$tgt_tool")"
is "and the hyphen is still there with no version" 1 \
    "$([ "$(setting "$PROJ" VERSION_INFO_STRING -sdk macosx -target "$tgt_tool")" = \
        "$(setting "$PROJ" VERSION_INFO_STRING -sdk macosx -target "$tgt_tool)-")" ] && echo 0 || echo 1)"
is "a target with a version ends with it" \
    '"@(#)PROGRAM:xcodebuildLoader  PROJECT:IDETools-0.1.0"' \
    "$(setting "$PROJ" VERSION_INFO_STRING -sdk macosx -target "$tgt_lib")"

is "XPCSERVICES_FOLDER_PATH is one fixed path" /XPCServices \
    "$(setting "$PROJ" XPCSERVICES_FOLDER_PATH -sdk macosx -target "$tgt_lib")"

# These are defaults, so an output override still wins -- the same precedence
# as the caches.  Checked on the two whose derivation is not a lookup of a
# value the project already set.
is "an output override beats the derived name" Other \
    "$(setting "$PROJ" PROJECT PROJECT=Other -sdk macosx)"
is "and the derived GUID" deadbeef \
    "$(setting "$PROJ" PROJECT_GUID PROJECT_GUID=deadbeef -sdk macosx)"

# ------------------------------------------------------------------
# -showBuildSettingsForIndex
# ------------------------------------------------------------------
#
# The index service is answered per source file rather than per target: one
# record per file, holding the arguments it wants the compiler to see.  Every
# expectation below is read off the project's own settings or its own pbxproj,
# so a flag that stops following its setting fails here rather than agreeing
# with itself.

echo "-showBuildSettingsForIndex"

# idx <project> [extra args...] -> the index object, echo and target headers
# removed.  The console form opens with the command it ran and names each
# target before its object, as -showBuildSettings does; everything below wants
# the object alone.  The -json form has neither, and starts at its brace.
idx() {
	_p=$1; shift
	"$TOOL" -project "$_p" -showBuildSettingsForIndex "$@" 2>/dev/null |
	    awk '/^Build settings for target/ { next } /^\{$/ { keep = 1 } keep'
}

# idx_walk <project> [extra args...] -> the target names the console form names,
# in the order it names them.  The header is the only place a target's name
# appears outside its own object, so this is the order of the walk.
idx_walk() {
	_p=$1; shift
	"$TOOL" -project "$_p" -showBuildSettingsForIndex "$@" 2>/dev/null |
	    sed -n 's/^Build settings for target \(.*\):$/\1/p'
}

# idx_rec <project> <source-basename> [extra args...] -> one source's record as
# `key = value` lines, each list joined by spaces.  Read from the -json form,
# where a source sits at four spaces and its keys at six, so the eight-space
# arguments and the closing `],` cannot be mistaken for the next record.  A
# source is addressed by basename because the key is an absolute path.
idx_rec() {
	_p=$1; _b=$2; shift 2
	"$TOOL" -project "$_p" -showBuildSettingsForIndex -json "$@" 2>/dev/null |
	    awk -v want="$_b" '
		/^    "/ {
			k = $0; sub(/^    "/, "", k); sub(/" : \{$/, "", k)
			inside = (k ~ ("/" want "$"))
			key = ""; buf = ""
			next
		}
		/^    \}/ { inside = 0; next }
		!inside { next }
		/^      "[^"]*" : \[$/ {
			key = $1; gsub(/"/, "", key); sub(/ : \[$/, "", key)
			buf = ""; n = 0
			next
		}
		/^      \]/ { if (key != "") { print key " = " buf; key = ""; buf = "" } next }
		/^        / {
			v = $0; sub(/^ *"/, "", v); sub(/",?$/, "", v)
			buf = (n++ == 0) ? v : buf " " v
			next
		}
		{
			key = $1; gsub(/"/, "", key); sub(/ : $/, "", key)
			v = $0; sub(/^[^:]*: /, "", v); gsub(/^"|",?$/, "", v)
			print key " = " v
		}
	'
}

# The six keys, in the order Apple writes them.  Order is the point: a reader
# diffing two runs reads it, and a key moved is a change no test of the values
# would have noticed.
is "a record names its keys in Apple's order" \
    "assetSymbolIndexPath clangASTBuiltProductsDir clangASTCommandArguments LanguageDialect outputFilePath toolchains" \
    "$(idx_rec "$PROJ" index.c -target "$tgt_tool" | sed 's/ = .*//' | paste -sd' ' -)"

# Order alone cannot catch two keys' headers trading places, since the six
# names would still be in the six positions.  What such a swap breaks is which
# value belongs to which name, so each of the two is checked against what it
# should hold: the dialect is a language identifier, the built-products
# directory the products directory.
rec=$(idx_rec "$PROJ" index.c -target "$tgt_tool")
is "LanguageDialect holds the language, not a path" \
    Xcode.SourceCodeLanguage.C "$(printf '%s\n' "$rec" | sed -n 's/^LanguageDialect = //p')"
is "clangASTBuiltProductsDir holds the products directory" \
    "$(setting "$PROJ" CONFIGURATION_BUILD_DIR -sdk macosx)" \
    "$(printf '%s\n' "$rec" | sed -n 's/^clangASTBuiltProductsDir = //p')"

# One record per source the target compiles.  Counted from the pbxproj's own
# build-file entries, which appear once per definition and once per phase, so
# the names are made unique first and the loader's own file left out: the count
# cannot follow the tool's own tally.
nphase=$(sed -n 's/.*\/\* \([A-Za-z0-9_]*\.c\) in Sources \*\/.*/\1/p' \
    "$PROJ/project.pbxproj" | grep -v '^xcodebuildLoader\.c$' | sort -u | awk 'END { print NR }')
is "one record per source file in the project" "$nphase" \
    "$(idx "$PROJ" -target "$tgt_tool" -json | grep -c '^    "/')"

# The object path is the target's own build directory, not a fixed one: a
# record that named a hardcoded directory would pass whatever OBJROOT was, and
# one that left out the target's own name would pass only for a target called
# xcodebuild.
is "an object's path is the target's own directory under the build" \
    "$(setting "$PROJ" CONFIGURATION_TEMP_DIR -sdk macosx)/$tgt_tool.build/Objects-normal/$(setting "$PROJ" ARCHS -sdk macosx)/index.o" \
    "$(idx_rec "$PROJ" index.c -target "$tgt_tool" | sed -n 's/^outputFilePath = //p')"

args=$(idx_rec "$PROJ" index.c -target "$tgt_tool" | sed -n 's/^clangASTCommandArguments = //p')
wordafter() { printf '%s' "$args" | sed -n "s/.*$1 \\([^ ]*\\).*/\\1/p"; }

# The three arguments that name where and how to compile.  Each is checked
# against the setting it is derived from, so these fail if the derivation
# breaks rather than if a path moves.  None of them names an -sdk: the index
# build is asked for the project's own SDK, and naming one here would make the
# two sides resolve different SDKs and agree about neither.
is "-isysroot names the SDK the settings resolved" \
    "$(setting "$PROJ" SDKROOT)" "$(wordafter -isysroot)"
is "-target carries the arch and the deployment target" \
    "$(setting "$PROJ" ARCHS -sdk macosx)-apple-macos$(setting "$PROJ" MACOSX_DEPLOYMENT_TARGET -sdk macosx)" \
    "$(wordafter -target)"
is "-x names the source language" "-x c" \
    "$(printf '%s' "$args" | cut -d' ' -f1,2)"
is "the language standard follows GCC_C_LANGUAGE_STANDARD" \
    "-std=$(setting "$PROJ" GCC_C_LANGUAGE_STANDARD -sdk macosx)" \
    "$(printf '%s' "$args" | tr ' ' '\n' | grep -e '^-std=')"

# The optimization and debug flags are settings restated, so they are compared
# as such rather than against a constant that would also pass if the flag were
# hardcoded the other way.
is "the optimization flag is the level restated" \
    "-O$(setting "$PROJ" GCC_OPTIMIZATION_LEVEL -sdk macosx)" \
    "$(printf '%s' "$args" | tr ' ' '\n' | grep -e '^-O[0-9gs]$' | head -1)"
is "-g is present exactly as DEBUGGING_SYMBOLS asks" \
    "$([ "$(setting "$PROJ" DEBUGGING_SYMBOLS -sdk macosx)" = YES ] && echo 1 || echo 0)" \
    "$(printf '%s' "$args" | tr ' ' '\n' | grep -cx -e '-g')"

# -fvisibility=hidden follows the kind of product, not a setting either
# project target says anything about.  A tool compiles with it, a dylib
# without, and the two are the only products in this project, so a rule keyed
# on the product and one keyed on nothing at all are told apart here.
is "a tool target compiles with -fvisibility=hidden, once per source" \
    "$nphase" "$(idx "$PROJ" -target "$tgt_tool" | grep -c 'fvisibility=hidden')"
is "a library target compiles without it" 0 \
    "$(idx "$PROJ" -target "$tgt_lib" | grep -c 'fvisibility=hidden')"

# An index build is answered from the project and the build record, not from
# the command line.  The -showBuildSettings half of each pair is what makes the
# test mean something: it shows the argument is one the tool does honour, and
# honours elsewhere.
is "-showBuildSettings does honour -configuration" Debug \
    "$(setting "$PROJ" CONFIGURATION -configuration Debug)"
is "and the index build ignores it" \
    "$(idx "$PROJ" -target "$tgt_tool")" \
    "$(idx "$PROJ" -target "$tgt_tool" -configuration Debug)"

is "-showBuildSettings does honour KEY=VALUE" 0 \
    "$(setting "$PROJ" GCC_OPTIMIZATION_LEVEL GCC_OPTIMIZATION_LEVEL=0 -sdk macosx)"
is "and the index build ignores it" \
    "$(idx "$PROJ" -target "$tgt_tool")" \
    "$(idx "$PROJ" -target "$tgt_tool" GCC_OPTIMIZATION_LEVEL=0 OTHER_CFLAGS=-DZZZ)"

# The walk over every target.  The console form ends by naming the first target
# again, as -showBuildSettings does; -json has no use for a repeated key and
# orders its targets by name instead, which is a different order from the
# project's own.
is "the console walk ends by naming the first target again" \
    "$tgt_tool $tgt_lib $tgt_tool" \
    "$(idx_walk "$PROJ" -alltargets | paste -sd' ' -)"
is "-json names each target once, in order by name" \
    "$(printf '%s\n%s\n' "$tgt_lib" "$tgt_tool" | sort | paste -sd' ' -)" \
    "$(idx "$PROJ" -alltargets -json | sed -n 's/^  "\(.*\)" : {$/\1/p' | paste -sd' ' -)"

# A target whose sources are all absent gets a blank object, not {}.  The Two
# fixture's files are not on disk, which is the case: a record that cannot be
# filled is left empty rather than invented.
is "a target with no files on disk is a blank object" \
    "$(printf '{\n\n}')" "$(idx "$ROOT/tests/fixtures/Two.xcodeproj" -target hello)"

echo
printf '%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
