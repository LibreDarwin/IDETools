# IDETools — product inventory

What belongs in Apple's **IDETools** project, what state each item is in, and
what it would take to build it. Companion to `docs/DOCUMENTATION.md`, which
holds the wider investigation (XCBuild, SwiftBuild, xcselect, the tree).

Scope note: this repo was renamed from `XCBuild` to `IDETools` in `86ee59b`,
because Apple's real `xcodebuild` carries `PROJECT:IDETools-24902` and has
nothing to do with Apple's XCBuild project (`PROJECT:xcbuild-24900.0.3`).

## How the inventory was derived

Every executable in `Contents/Developer/usr/bin`, `Contents/SharedFrameworks`,
`Contents/Frameworks` and `Contents/PlugIns` was scanned for the
`PROJECT:IDETools-` marker. **Exactly three products carry it.** Nothing else in
Xcode.app belongs to IDETools.

```sh
for d in Contents/Developer/usr/bin Contents/SharedFrameworks \
         Contents/Frameworks Contents/PlugIns; do
  find /Applications/Xcode.app/$d -maxdepth 4 -type f -perm -u+x 2>/dev/null | while read -r f; do
    strings -a "$f" 2>/dev/null | grep -q 'PROJECT:IDETools-' && echo "$f"
  done
done
```

## The three products

| # | Product | Size | Status here |
| --- | --- | --- | --- |
| 1 | `Developer/usr/bin/xcodebuild` | 100,528 | **built** (`build/$(CONFIG)/xcodebuild`) — and it builds its own project |
| 2 | `Contents/Frameworks/libxcodebuildLoader.dylib` | 73,440 | **built** (`build/$(CONFIG)/libxcodebuildLoader.dylib`) |
| 3 | `Developer/usr/bin/xcindex-test` | 383,584 | out of reach (see below) |

### 1. `xcodebuild` — the tool

The main deliverable. Built by both `Makefile` and `IDETools.xcodeproj`; see
`docs/DOCUMENTATION.md` §5 and §8 for duplication status against the tree and
the build details.

Apple's version is ObjC + Swift and links Foundation, CoreFoundation, libobjc and
`DVTSystemPrerequisites`. Ours is C and links CoreFoundation + libSystem only, so
byte-identity is out of reach — the achievable goal is behavioural parity.

#### It builds its own project

Since `ba8ef6c` the tool builds `IDETools.xcodeproj` — not just compiles, links
and produces a working binary, but produces a binary that builds the same project
to a **byte-identical** result. That fixed point is the strongest evidence
available here: it says the tool and `make` agree about what these sources
compile to, which is the property the whole clean-room exercise is for.

The three faults that stood in the way were all pre-existing, none of them
visible from the output, and all three were found only by attempting the
self-build:

| Fault | Symptom | Why it hid |
| --- | --- | --- |
| A bare `-project Foo.xcodeproj` set `SRCROOT` to the `.xcodeproj` *file* | `'cfplist.h' file not found` | `strrchr(path, '/')` returns NULL, so "cut at the last slash" never cuts. A path that looks well formed, under a file. |
| Array build settings were never expanded | same | `merge_entry()` expanded string values and joined arrays raw. `HEADER_SEARCH_PATHS` is an array. |
| `OTHER_CFLAGS` / `OTHER_LDFLAGS` were never read | undefined `_kCFString*` at link | settings merged, stored, displayed — and had no effect on any command. |

Two of these are worth carrying to the tree, which has its own copies of the same
three files:

- The bare-name `SRCROOT` cut and the array expansion both live in code the tree
  also has (`xcodebuild.c`, `settings.c`). Not yet fixed there.
- The export-options plist is still scraped as text with `read_file_all` +
  `xmlplist_get` in the tree's `xcodebuild.c`, the fault fixed here in `41c4576`.

`xcpath.c` is the small addition from the first row: `xc_dirname()` in
`src/common/`, shared by the three sites that took a directory off a path, because
`project.c` had the correct `else → "."` branch written down and the other two did
not follow it.

### 2. `libxcodebuildLoader.dylib` — relaunch trampoline

Fully specified below. It is small (73 KB on disk, only **3,412 bytes of
machine code** across `__text` 0xd54 + `__objc_stubs` 0x540), self-contained,
and was the most tractable of the three. **Built** in `70f52aa`; the
specification below is retained because the two substitutes that were not
predictable — a dylib cannot recover its host's argv, and `MacOSX.Internal.sdk`
declares `@interface NSString` with none of its methods — are the reason it is C
and not Objective-C, and neither is visible in the disassembly.

```sh
otool -D    /Applications/Xcode.app/Contents/Frameworks/libxcodebuildLoader.dylib
nm -a       /Applications/Xcode.app/Contents/Frameworks/libxcodebuildLoader.dylib
otool -tV    /Applications/Xcode.app/Contents/Frameworks/libxcodebuildLoader.dylib
```

Shape:

```
install name:  @rpath/Frameworks/libxcodebuildLoader.dylib
arch:          arm64
PROJECT:       IDETools-24902
links:         @rpath/Xcode3Core.framework/Versions/A/Xcode3Core
               @rpath/DVTFoundation.framework/Versions/A/DVTFoundation
               Foundation, libobjc.A, libSystem.B

exports:
  _XcodeBuildMain
  _LoadAddressSanitizerLibrariesIfPresentAndRelaunch
  _xcodebuildLoaderVersionNumber
  _xcodebuildLoaderVersionString
```

Only two real functions (`__text` 0x960–0x16d4), with 6 `.cold` blocks on the
ASan one and 2 outlined helpers on `XcodeBuildMain`. The 73 KB is almost
entirely `__DATA`: ObjC metadata, 60-odd `objc_msgSend$*` selector stubs, and
`__cstring` (0x4f3 = 1,267 bytes of literals).

#### Public entry point

```c
BOOL XcodeBuildMain(BOOL allowASanRelaunch, NSString *commandName,
                    NSString *bundleID, NSString *logAspectName,
                    NSString *timelineTraceFormat);
```

Reconstructed from the prologue: args land in `x19`–`x22` (all four retained via
`objc_retain`), `w23` holds arg 0 and is tested with `cbz` at `0x1300` — if it
is zero the relaunch is skipped and it goes straight to argument logging.

Order of operations in `XcodeBuildMain`:

1. If `allowASanRelaunch`, call
   `LoadAddressSanitizerLibrariesIfPresentAndRelaunch(0, commandName)`.
2. `[commandName dvt_stringByConcatenatingAsCommandLineArguments]` — the joined
   argv, logged as `xcodebuild starts [args: %@]`.
3. `DVTTimelineBeginSubactivity(timelineTraceFormat, args)`.
4. `[NSUserDefaults standardUserDefaults] registerDefaults:` with
   `PBXBuildDebugLevel = 2` and `_CFForceASCIICompatibility = YES`.
5. `DVTSetupWeakPropertyKVOAssertions()`.
6. `DVTEnvironmentSnapshotString(<bundleID>)`, logging
   `timeline trace info (for internal diagnostic purposes; if you don't know what
   this is, please ignore it):` when set.
7. Hand off to `[Xcode3CommandLineBuildTool sharedCommandLineBuildTool]`, run it
   as a task with `setName:` / `setArguments:` / `setEnvironment:` /
   `setStandardInput:` / `setStandardOutput:` / `setStandardError:`, honouring
   the `XcodebuildClearEnvironmentVariables` default, then log
   `xcodebuild exits [status: %i]`.

Arguments are joined with `,` (`0x21a8`).

#### The relaunch mechanism

The name is misleading: it does **not** primarily use `DYLD_INSERT_LIBRARIES`.
It sets **`DYLD_IMAGE_SUFFIX=_asan`** and `execv`s itself. dyld then resolves
`…/libxcodebuildLoader.dylib` to `…_asan.dylib` and `DVTFoundation.framework` to
`DVTFoundation_asan`, i.e. Apple ships ASan-instrumented twins of its own
frameworks in internal builds and the suffix picks them up. `DYLD_INSERT_LIBRARIES`
is the secondary path, used for the Clang ASan runtime.

Supported inputs, all read through `DVTEnvironmentSnapshot*` (DVTFoundation's
env accessor) or `NSUserDefaults`:

| Input | Kind | Effect |
| --- | --- | --- |
| `DVTNoASanRelaunch` | env + default | hard skip: `Skipping ASan relaunch because DVTNoASanRelaunch is set` |
| `DVTForceASanRelaunch` | default | force even when not the default |
| `FORCE_ASAN_RELAUNCH` | env | same, via `DVTEnvironmentSnapshotBool` |
| `DVTASanRelaunch` | default | opt in |
| `disable-asan-relaunch` | default | `Skipping ASan relaunch because it is not the default and a force relaunch was not requested` |
| `DYLD_IMAGE_SUFFIX` | env | if already `_asan`: `Skipping ASan relaunch because DYLD_IMAGE_SUFFIX is already set to '_asan'` |

Guard clauses, in the order their literals appear in `__cstring`:

1. `dlsym` for `DVTFoundationErrorDomain` —
   `Skipping DVTFoundationErrorDomain symbol is missing`.
2. Probe for a `DVTFoundation_asan` twin, path built with the `%s_asan` format
   from a directory resolved via `/Applications/Xcode.app/`, `lib/darwin` and
   `Xcode`: `Skipping ASan relaunch because DVTFoundation_asan does not exist at %@`.
3. Recognise the host binary — `xcodebuild` under `Contents/Developer/usr/bin`,
   checked with `rangeOfString:` / `hasPrefix:` / `isEqual:` against
   `[mainBundle executablePath] stringByDeletingLastPathComponent`:
   `Not relaunching under ASan because binary is not recognized as supporting doing this: %@`.
4. Locate the runtime with `DVTMachORPathsForExecutable`:
   `Cannot find path to ASan dylib in binary: %@`.

Then, on success:

```
Relaunching %@ with DYLD_IMAGE_SUFFIX=_asan
-> DYLD_INSERT_LIBRARIES=%@                       (or)
-> Not setting DYLD_INSERT_LIBRARIES because ASan dylib does not exist at path: %@
Failed to relaunch %@ with DYLD_IMAGE_SUFFIX=_asan, errno=%d
```

The injected dylib is `libclang_rt.asan_osx_dynamic.dylib`. Confirmed present in
a public Xcode at
`Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/21/lib/darwin/`.

#### What a public Xcode actually does

The `_asan` twins **do not ship in a public Xcode** — there is no
`DVTFoundation_asan` anywhere under `DVTFoundation.framework`, and no
`libxcodebuildLoader_asan.dylib` under `Contents/Frameworks`. So guard 2 stops
the relaunch, and with no opt-in set the default path is the "not the default"
skip. The relaunch machinery is effectively dormant outside Apple's internal
builds; `libclang_rt.asan_osx_dynamic.dylib` is the only part reachable here.

#### Dependencies we do not have

`DVTFoundation` and `Xcode3Core` do not exist in this tree and are large IDE
frameworks. A faithful port therefore has to replace them:

| Apple | Substitute here |
| --- | --- |
| `DVTEnvironmentSnapshot{Bool,String}` | `getenv` |
| `DVTSetEnvironmentVariable` / `DVTRemoveEnvironmentVariable` | `setenv` / `unsetenv` |
| `[NSUserDefaults boolForKey:]` | `CFPreferencesCopyAppValue` + `CFBooleanGetValue` |
| `DVTMachORPathsForExecutable` | own Mach-O `LC_LOAD_DYLIB` scan |
| `DVTTimeline{Begin,End}Subactivity` | own logging |
| `DVTLogAspect` | own logging |
| `DVTFoundationErrorDomain` `dlsym` probe | drop, or probe a symbol we do define |
| `[Xcode3CommandLineBuildTool sharedCommandLineBuildTool]` | not needed — our `main()` is the tool |

The observable contract — variable names, messages, decision order, exit-status
propagation — is reproducible with Foundation and libSystem only.

#### What we actually had to add beyond that table

Two things the substitute list could not have predicted, both found by building
it:

**A dylib cannot recover its host's argv.** The relaunch has to replay the
original argument vector, and there is no portable way to get it. `_NSGetArgc`
and `_NSGetArgv` still link and still resolve on modern macOS — and return
garbage, `argc` came back as `-165134544`. `crashreporter.h`, which declares
them, is absent from `MacOSX.Internal.sdk` entirely. So the tool hands its argv
over explicitly, which is a third exported entry point with no Apple counterpart:

```c
void XcodeBuildSetInvocation(int argc, char * const *argv);
```

Until it is called the loader has nothing to replay, and it declines to relaunch
rather than silently dropping the tool's arguments.

**The tool does not link the loader — it `dlopen`s it.** Apple's `xcodebuild`
carries the literal string `@rpath/libxcodebuildLoader.dylib` and calls
`dlopen`/`dlsym`; `nm -u` shows it does *not* import `_XcodeBuildMain`, and
`otool -L` shows it links only `DVTSystemPrerequisites.framework`. Its three run
paths exist for that framework, of which we need one:

| Run path | Purpose |
| --- | --- |
| `@executable_path/../../..` | Apple's, for DVT prereqs — not needed here |
| `@executable_path/../../../Frameworks` | **ours: resolves the loader** |
| `@executable_path/../../../SharedFrameworks` | Apple's, not needed here |

Our tool also carries `@loader_path` first, so the in-tree build finds the
loader as a sibling in `build/release/`. Linking the loader instead would make
the tool refuse to start on any machine where the second product was not
installed next to it, so the `dlopen` is deliberate and the hand-off is
best-effort: a missing or incomplete loader is silent, and the tool still builds.

The hand-off is placed just before the build dispatch rather than at the top of
`main()`, so `--version`, `--help` and `-showBuildSettings` never pay for a
re-exec.

#### Language: C, not Objective-C

`MacOSX.Internal.sdk` declares `@interface NSString : NSObject` with **none of
its methods**, so every `[nsstring someMethod]` in an Objective-C loader is an
undeclared selector against this SDK. The loader is therefore plain C, like the
rest of the tree. The `NSString *` arguments are declared `CFStringRef` — the two
are toll-free bridged, so the ABI is Apple's exactly. This drops the `libobjc`
link that Apple's loader has.

#### Install layout is load-bearing

`PREFIX` names the **Developer** directory, but the loader is not inside it. It
is a sibling:

```
$XCODE/Contents/Developer/usr/bin/xcodebuild              <- PREFIX=/usr/bin
$XCODE/Contents/Frameworks/libxcodebuildLoader.dylib       <- CONTENTS_DIR=/Frameworks
```

`$(PREFIX)/Frameworks` would be `Contents/Developer/Frameworks`, which is off
the `../../../Frameworks` run path and where nothing can load it. The Makefile
therefore derives `CONTENTS_DIR ?= $(PREFIX)/..` separately.

#### Verification

Exports match Apple's name-for-name, with the same symbol types:

| Symbol | Apple | Ours |
| --- | --- | --- |
| `_XcodeBuildMain` | `T` | `T` |
| `_LoadAddressSanitizerLibrariesIfPresentAndRelaunch` | `T` | `T` |
| `_xcodebuildLoaderVersionString` | `S` | `S` |
| `_xcodebuildLoaderVersionNumber` | `S` | `D` (ours is a plain `double` in `__DATA`) |
| — | — | `T` — `XcodeBuildSetInvocation`, ours alone |

Install name `@rpath/Frameworks/libxcodebuildLoader.dylib` and compatibility /
current version `1.0.0` both match Apple. Verified end to end from a staged
`Contents/Developer` layout: the runtime is found, `DYLD_INSERT_LIBRARIES` and
`DYLD_IMAGE_SUFFIX=_asan` are set, `execv` replaces the image, and the second
process declines to relaunch again. The no-runtime case reports
`Cannot find path to ASan dylib in binary` and proceeds. Toolchain scan picks
the higher-sorting clang version deterministically over repeated runs (Xcode
ships both `21` and `21.0.0`).

### 3. `xcindex-test` — out of reach

383,584 bytes linking AppKit, libedit, Foundation, `DVTFoundation`,
`IDEFoundation` and a wide `libswift*` set including RegexBuilder and
StringProcessing. It is a test harness for the source index and depends on IDE
frameworks we have no path to. Record it as IDETools-owned but **not a
LibreDarwin goal** unless the index work is ever picked up.

## Explicitly not ours

| Item | Marker | Why not |
| --- | --- | --- |
| `SharedFrameworks/DVTSystemPrerequisites.framework` | *(none)* | first-launch package/licence handling, links PackageKit. Linked by Apple's `xcodebuild`, so it blocks byte-identity, but it belongs to no IDETools build. |
| `/usr/bin/xcodebuild` | *(none — shim)* | `xtool-shim-public`, belongs to xcselect |
| `XCBuild.framework`, `SwiftBuild.framework`, `XCBBuildService.bundle` | `xcbuild-24900.0.3` | Apple's XCBuild project — deferred, see `docs/DOCUMENTATION.md` §7 |

## Neighbouring projects

Apple's DeveloperTools is ~33 separate projects, one repo each, so LibreDarwin's
`DeveloperTools/` should mirror that split rather than absorb them:

```
ACPXcode            AppThinning         BackgroundAssets     CarbonTools
ContentDelivery     CoreUI              DevToolsCore          DVTFrameworks
DVTiOSFrameworks    FileMerge           GamePolicy            IDEMLKit
IDEFrameworks       IDEIntentBuilder    IDEInterfaceBuilder   IDEPlugIns
IDESafariTools      IDESceneKitEditor   IDESpriteKitSupport   IDESwiftPackageSupport
IDETools            Instruments         MobileInstallation    RealityTools
SamplingTools       SiriSSUKit          XCResultKit           XCResultKitBNI
XCTest              genstrings          gnumake               libticket
libtrace
```

The small, self-contained neighbours worth considering as separate repos later:
`gnumake`, `libtrace`, `libticket`, `genstrings`.

## Plan

1. ✅ `xcodebuild` — builds, `xcodebuild 1.0.0`, both make flavours and Xcode, and
   builds its own project to a byte-identical binary.
2. ✅ `libxcodebuildLoader.dylib` — implemented in `src/loader/` as C over
   CoreFoundation, exported surface matches Apple, and the relaunch is verified
   end to end from a staged layout. `IDETools.xcodeproj` builds it as a second
   target, and the two build systems agree on arch, run paths and product names.
3. ✅ `src/common/sdkpath.{c,h}` backported to the tree's
   `src/openxc-tools/common/` — tree `ecd0f70`, which shares the property-list
   reader out into `cfplist.{c,h}` so a tool with a plist to read need not write
   another. `sdkpath.c` and `sdkpath.h` are now byte-identical in both trees.
4. ✅ The three faults in §1 fixed in the tree as well, one commit each:
   `3834dc1` export-options plist through `cfplist`, `daf6fc5` `xc_dirname` at
   the three sites, `95d6d02` array settings expanded, `82a443f` `OTHER_CFLAGS`
   and `OTHER_LDFLAGS` honoured. The tree's `xcodebuild` now builds
   `IDETools.xcodeproj` to a fixed point.
5. ✅ Physical checkout rename to `IDETools` and the GitHub remote rename
   (`origin` → `LibreDarwin/IDETools.git`; the old name is kept as `old`).
6. ⬜ Open: whether `openxc-tools/xcodebuild` retires in favour of this repo. Not
   decided, and deliberately not acted on.
7. ❌ `xcindex-test` — recorded, not planned.

#### Status verdict — 2026-09-28

Re-assessed at `ba8ef6c` (this repo) and `82a443f` (tree), with the duplication
audit re-measured rather than carried forward:

| Scope | Verdict |
| --- | --- |
| Reachable products (1 and 2) | **complete** — built, verified, and self-building |
| Product 3, `xcindex-test` | **out of scope**, not unfinished — links IDE frameworks with no path to them |
| Behavioural parity with Apple | **not met, now measured** — 325 differing lines in `-showBuildSettings`; the two outright bugs it exposed are fixed |
| Retirement of `openxc-tools/xcodebuild` | **open decision** — needs a go-ahead, not more work |
| `../xcselect` | **untouched**, awaiting the go-ahead |

So: no, the project is not finished in the sense of "all three products built",
and it is not unfinished in the sense of "work remaining on what we can reach".
Both reachable products are done. What remains is real work, not bookkeeping:
close the parity deltas above, then a scope decision (`xcindex-test`,
retirement) and two go-aheads (`xcselect`, retirement).

The distinction that matters when reading the earlier sections: the tree gap
list shrank from four faults to zero, but it did not shrink because the tool
improved — the remaining differences are deliberate (the loader product is ours
alone; the tree has no C++ sources to need `OTHER_CPLUSPLUSFLAGS`). Nothing in
`docs/DOCUMENTATION.md` §5 is a pending backport. The parity gap is the
opposite case: it was never measured, and now that it has been, it is the
largest piece of outstanding work in the project.

#### The tree builds this project

With the four faults fixed, the tree's own `xcodebuild` builds
`IDETools.xcodeproj` end to end — nine sources, one link, no warnings — and the
binary it produces builds the same project again to a **byte-identical**
result. That is the same fixed point §1 records for this repo's tool, reached
from the other side, and it is the strongest single piece of evidence that the
two implementations agree about what these sources compile to.

The chain is worth recording because each step alone looked fine. The array
fault compiled nothing; the `OTHER_LDFLAGS` fault compiled all nine sources
and then failed on `_CFArrayCreateMutable` and `_kCFString*`, which is what
`-framework CoreFoundation` in `OTHER_LDFLAGS` would have supplied. Neither
error mentioned the setting that caused it.

`OTHER_CPLUSPLUSFLAGS` was left unwired in the tree on purpose: its `build.c`
compiles `.cc`/`.cpp`/`.cxx`/`.mm` through the same path as `.c` and has no
`is_cxx()` to branch on, so there is nowhere for a C++-only branch to mean
anything. That is a change to how the tool compiles, not a fix to a setting it
drops.

#### What neither implementation does

Three settings the project sets are read by **neither** implementation, so an
earlier draft of this file wrongly charged all three to the tree:

| Setting | In `project.pbxproj` | Read by our `build.c` | Read by the tree's `build.c` |
| --- | --- | --- | --- |
| `GCC_OPTIMIZATION_LEVEL` | `0` Debug, `3` Release | no | no |
| `DEBUG_INFORMATION_FORMAT` | `dwarf` | no | no |
| `-derivedDataPath` | n/a (an option) | parsed, never read | parsed, never read |

`GCC_OPTIMIZATION_LEVEL` and `DEBUG_INFORMATION_FORMAT` appear only as entries
in the defaults table (`settings.c`, byte-identical in both trees); `build.c`
mentions neither, and contains no `-O0` and no `-g` literal. They are therefore
**shared** gaps, not tree-only ones. Note also that the difference they were
assumed to explain is not attributable to them: no `-O0` is emitted on either
side, so optimisation level cannot account for a size delta.

`-derivedDataPath` is parsed and stored (`opts->derived_data_path`,
`xcodebuild.h:113`), freed at teardown, and never read. Output goes to the
project's `CONFIGURATION_BUILD_DIR` (`xcodebuild.c:531`, consumed at
`build.c:2987`).

Reproducible sizes, release against release: our `make CONFIG=release` product
is 178,184 bytes, the tree's `build/release` product 188,120. Both are
`CoreFoundation` + `libSystem` only. The two products differ because the tree
builds the loader-free, `OTHER_CPLUSPLUSFLAGS`-free variant and links its own
`common/` copies — not because of a missing setting. Our figure was 177,944
before the two fixes below; the 240 bytes are the default-configuration helper
and the SDK sync, so the size difference is not build flags either.

`../xcselect` is untouched and still waits for the go-ahead.

#### Behavioural parity is a goal, and it is not met

Byte-identity is unreachable (§1). Behavioural parity is the substitute goal,
and it was **assumed** rather than measured. Measuring it on 2026-09-28 against
Apple's `xcodebuild -showBuildSettings` for this project:

```
settings emitted   apple 460, ours 418
  shared            367
  only in Apple      93
  only in ours       51
  shared, differing  43
  diff lines:       325 of a 465-line output
```

(Count settings, not lines: raw line counts shift by a few depending on how the
trailing newline is handled, and a strict four-space regex silently drops keys.
The figures above come from matching `^\s*KEY = value` on either side; they
reconcile: 93 + 367 = 460 and 51 + 367 = 418.)

These are the figures *after* the two bugs below were fixed. Before them it was
ours 419, only-in-ours 52, differing 49, 327 diff lines — the extra setting was
`GCC_PREPROCESSOR_DEFINITIONS`, which is `DEBUG=1` under Debug and absent under
Release in both tools, so its disappearance is the configuration fix working,
not a regression.

The 419-line figure that earlier drafts of this file called "byte-identical to
Apple" is real, but it is **ours against the tree's** `xcodebuild` — the two
LibreDarwin tools agree exactly. It was never an Apple comparison, and reading
it as one is what hid this gap. Ours vs the tree re-verified today: 419 lines
each, byte-identical, same resolved `SDKROOT`, `DEVELOPER_DIR` and
`PLATFORM_DIR`.

#### Two bugs found and fixed

Both were found by the measurement above, and both are fixed in the working
tree:

| | was | now | why it mattered |
| --- | --- | --- | --- |
| default configuration | `Debug` | `Release` | with no `-configuration`, the same project built differently under the two tools — and worse, *our own* tool disagreed with itself: `-list` reported the project's `defaultConfigurationName` while the build silently used `Debug` |
| `SDK_VERSION` | `15.4` | `26.5` | the project hardcodes `SDKROOT` to `MacOSX.Internal.sdk`. We honoured `SDKROOT` but derived `SDK_DIR`/`SDK_NAME`/`SDK_VERSION` from the *default* scan (`/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk`), so the build compiled against one SDK while claiming another |

The configuration fix settles the default from the project's own
`defaultConfigurationName`, which is what `-list` was already reporting — one
answer now, read through one function (`project_default_configuration()`), with
`"Release"` left as the fallback for a project that names none. It has to
happen before the defaults load, because the name goes into
`BUILT_PRODUCTS_DIR`, the temporary directories and `CONFIGURATION`.

The SDK fix adds `settings_sync_sdk_root()`, which re-reads the SDK that
`SDKROOT` names and takes its `Version` and `CanonicalName` from that SDK's own
`SDKSettings.plist` — the same read the defaults came from. It also derives
`SDK_VERSION_ACTUAL`/`MAJOR`/`MINOR` from the version so they cannot drift:
Apple's encoding is each part zero-padded to two digits and run together
(`26.5 → 260500`, `15.4 → 150400`), with `MAJOR` the major part padded to four
and `MINOR` truncated at the minor part. That rule was read off Apple's output
for three different SDKs rather than guessed.

One ordering change came with it. `SETTING=value` overrides used to be applied
*before* the developer directory's paths and the `SDKROOT` resolution, which
silently lost an explicit `SDKROOT=` to whichever SDK the scan had found. They
are now applied last, on both sides of the SDK sync, so an explicit setting on
the command line wins — including an explicit `SDK_VERSION=`, which the sync
would otherwise overrule.

Verified after the fix: `SDK_DIR`/`SDK_NAME`/`SDK_NAMES`/`SDK_VERSION` and the
three numbered forms match Apple for the project default and for `SDKROOT=`
pointed at both `MacOSX26.5.sdk` and `MacOSX15.4.sdk`; an explicit
`SDK_VERSION=99.1` and `SDK_VERSION_ACTUAL=123` still win; `-list` is unchanged
against Apple; and the self-build fixed point still holds byte-identically
(`197448` bytes, sha256 `288aabb35f14d7a9` — byte-for-byte the same artifact HEAD
produces when told `-configuration Debug` explicitly, so the fix changes what the
tool *reports* about a build, not how the build is compiled or linked). The
`xcodebuild.c` diff is ~190 lines of re-indent because the pbxproj had to be
parsed before the defaults load, which moved its one level of nesting; `-w` shows
the real change as 109 lines.

The rest of the gap is systematic, not incidental:

- **Paths are relative where Apple's are absolute** — `SRCROOT`/`PROJECT_DIR`
  `.` against `/Users/…/IDETools`, `BUILD_DIR` `./build` against an absolute
  path. Ours also uses `./build/Debug` where Apple uses
  `…/build/…/Release-iphoneos`-style derived directories.
- **`ARCHS_BASE` is `arm64 x86_64`, Apple says `arm64`** — we do not narrow to
  the host architecture.
- **`RPATH_ORIGIN` `@loader_path` vs `$ORIGIN`; `TOOLCHAINS` `MacOSX` vs
  `com.apple.dt.toolchain.XcodeDefault`; `DYNAMIC_LIBRARY_EXTENSION` `dylib`
  vs `so`.**
- **93 settings missing outright**, including `ANDROID_DEPLOYMENT_TARGET`,
  `CCHROOT`, `LEGACY_DEVELOPER_DIR`, `DUMP_DEPENDENCIES_OUTPUT_PATH`, and a
  family of `*_DEPENDENCY_INFO_FILE` / `*_MAP_FILE_PATH` linker settings.
- **52 we emit that Apple does not**, mostly code-signing and Clang-warning
  defaults (`AD_HOC_CODE_SIGNING_ALLOWED`, `CLANG_ENABLE_MODULES`,
  `CLANG_WARN_*`).

So the honest status is: **behavioural parity is not met, and the gap is
measured rather than guessed.** The two bugs that measurement exposed are fixed;
what remains is systematic. The `-showBuildSettings` surface still accounts for
325 differing lines, and of Apple's 460 settings, 93 are absent from ours and 43
more carry a different value — 136 of 460 do not match, before counting the 51
keys we emit that Apple never does. Certifying parity means diffing
`-showBuildSettings`, `-list` and the emitted compile/link command lines against
Apple's binary across a matrix of options and targets, and closing the deltas
above. Nothing in the fixed-point evidence substitutes for this: a fixed point
proves self-consistency, and ours agreeing with the tree's tool only shows two
implementations of the same `sdkpath.c` agree with each other.

The next candidates, in the order they are worth doing, all come from the same
measurement rather than from reading code:

- **`DEVELOPER_DIR` is `/Library/Developer/CommandLineTools`, Apple says
  `/Applications/Xcode.app/Contents/Developer`.** Everything derived from it —
  `DEVELOPER_SDK_DIR`, `DT_TOOLCHAIN_DIR`, `TOOLCHAIN_DIR`, the whole
  `PLATFORM_DEVELOPER_*` family — differs for this one reason. Ours is not
  *wrong* about itself, but it resolves the developer directory differently from
  Apple, and a caller relying on `$(DEVELOPER_DIR)` gets a different answer.
  This is also why a self-build needs `DEVELOPER_DIR` set explicitly: the
  CommandLineTools toolchain it resolves to has no `clang` in it, so a build
  fails at `build.c:3109` before compiling anything.
- **Relative vs absolute paths** (`SRCROOT`, `PROJECT_DIR`, `BUILD_DIR`), which
  affect every path in the settings and so every command line derived from them.
- **`ARCHS_BASE` not narrowed to the host architecture.**
- **The 93 absent settings**, which are mostly one family of linker
  `*_DEPENDENCY_INFO_FILE` / `*_MAP_FILE_PATH` defaults and the
  `SDK_STAT_CACHE_*` / `SDK_DIR_<name>` per-SDK conveniences rather than 93
  independent omissions.
