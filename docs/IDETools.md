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
current version `1.0.0` both match Apple — those are the dylib's install-name
versions, pinned there for the same reason, and they are not this project's
version. The exported `xcodebuildLoaderVersionString` was
`@(#)PROGRAM:xcodebuildLoader  PROJECT:IDETools-1.0.0`, which is Apple's shape
carrying this project's semver in Apple's field: in Apple, `PROJECT:` names one
Xcode build number shared by the product and the build, so `IDETools-24902` means
build 24902 of IDETools. Emitting `IDETools-1.0.0` claimed to be an IDETools
build in Apple's namespace at a version Apple never shipped. It now reads
`@(#)PROGRAM:xcodebuildLoader  PROJECT:LibreDarwin-0.1.0`, with
`CURRENT_PROJECT_VERSION` in `project.pbxproj` and the release tag all at
`0.1.0`, and `_xcodebuildLoaderVersionNumber` at `0.1`. Verified end to end from a staged
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

1. ✅ `xcodebuild` — builds, `xcodebuild 0.1.0`, both make flavours and Xcode, and
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
8. ✅ Regression tests in `tests/run.sh`, run by `make test` and `bmake test`.
   70 assertions over the bugs above plus the unresolvable-`SDKROOT` case they
   turned up, each checked against the pbxproj, an SDK's own plist, a
   toolchain's own plist, or a rule read off Apple rather than against the tool
   itself. Against `ba8ef6c` the same file fails 17, and against the working
   tree at `8bcd2ce` it fails 13, so it discriminates on each of them.

#### Status verdict — 2026-09-28

Re-assessed at `ba8ef6c` (this repo) and `82a443f` (tree), with the duplication
audit re-measured rather than carried forward:

| Scope | Verdict |
| --- | --- |
| Reachable products (1 and 2) | **complete** — built, verified, and self-building |
| Product 3, `xcindex-test` | **out of scope**, not unfinished — links IDE frameworks with no path to them |
| Behavioural parity with Apple | **not met, now measured** — of Apple's 458 settings, 67 absent, both counted per key with both tools on the same `DEVELOPER_DIR`; no value differences at all where the SDK resolves, the 6 on the default build being Apple's fallback for a missing SDK; the bugs the measurement exposed are fixed |
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
Apple's `xcodebuild -showBuildSettings` for this project, with both tools
pointed at the *same* Developer directory (`/Applications/Xcode.app/Contents/
Developer`). That agreement is deliberate rather than automatic: Apple ignores
`DEVELOPER_DIR` and reports the bundle it was launched from, so naming Apple's
own directory is what makes the two sides comparable at all.

```
settings emitted   apple 458, ours 442
  shared            391
  only in Apple      67
  only in ours       51
  shared, differing   6
```

(Count settings, not lines: raw line counts shift by a few depending on how the
trailing newline is handled, and a strict four-space regex silently drops keys.
The figures above come from matching `^\s*KEY = value` on either side; they
reconcile: 67 + 385 = 452 and 51 + 385 = 436, plus the 6 whose values differ,
which is 458 and 442 counting each differing key once on the shared side.)

Pointing our tool at a *different* Developer directory than Apple's inflates
this by 31, not 10, and the extra 21 arrived with the `SYSTEM_DEVELOPER_*` and
`PLATFORM_DEVELOPER_*` family described below. All 31 are correctly reporting
the directory each tool was given, so the matched comparison above is still the
one to read. `DEVELOPER_DIR` turns out to be a question the two tools answer
differently on purpose; see the subsection on it.

#### Six differences remain, and none of them are ours

The six that survive are all the same thing: this project hardcodes
`SDKROOT = MacOSX.Internal.sdk`, no such SDK exists, and Apple's toolchain falls
back to internal-SDK defaults when the SDK it was pointed at cannot be found.

```
  DYNAMIC_LIBRARY_EXTENSION              Apple=so          ours=dylib
  RPATH_ORIGIN                            Apple=$ORIGIN     ours=@loader_path
  PLATFORM_USES_DSYMS                     Apple=NO          ours=YES
  TAPI_VERIFY_MODE                        Apple=ErrorsOnly  ours=Pedantic
  PLATFORM_REQUIRES_SWIFT_AUTOLINK_EXTRACT Apple=YES        ours=NO
  PLATFORM_REQUIRES_SWIFT_MODULEWRAP      Apple=YES         ours=NO
```

Reproduce it: give both tools `-sdk macosx`, where the SDK resolves, and every
one of the six agrees.

```
settings emitted   apple 484, ours 442
  shared            413
  only in Apple      71
  only in ours       29
  shared, differing   0
```

Zero differing keys. The honest conclusion is that the six are not defects in
this tool but evidence that it does not reproduce a fallback for a missing SDK,
and hardcoding the fallback values would be a second way to be wrong — it would
break the resolvable-SDK case above, which currently matches. What is left to
do about the fallback itself is a separate question, deliberately not answered
here.

The 22 keys that moved from "only in ours" (51 → 29) between the two tables
are not SDK identity keys, which is what the first reading of the pair suggested.
They are the groups Apple's fallback suppresses: code signing
(`AD_HOC_CODE_SIGNING_ALLOWED`, `CODE_SIGNING_REQUIRED`, `CODE_SIGN_IDENTITY*`,
`ENTITLEMENTS_DESTINATION`), the sanitiser and TAPI flags (`KASAN_*`,
`TAPI_USE_SRCROOT`), the deployment-target suggestions
(`DEPLOYMENT_TARGET_SUGGESTED_VALUES`, `RECOMMENDED_MACOSX_DEPLOYMENT_TARGET`,
`SDK_PRODUCT_BUILD_VERSION`, `LLVM_TARGET_TRIPLE_OS_VERSION_*`), and a handful of
internal sentinels (`_BOOL_*`, `_IS_EMPTY_`, `_DEVELOPMENT_TEAM_IS_EMPTY`,
`_MACOSX_DEPLOYMENT_TARGET_IS_EMPTY`). We emit all of them regardless of whether
the SDK resolved; Apple emits none of them in the fallback. So on the default
build these 22 are, like the 6, evidence about Apple's fallback rather than
claims we have not checked — and the practical consequence is that the
"we emit keys Apple does not" list is 29 keys long on a build where the SDK
resolves, not 51.

#### Twenty keys that were one table, not twenty decisions

The next largest group in the Apple-only list was expected to be the cache and
linker paths, and the expectation was wrong. Counting by key-name stem put 14
`SYSTEM_DEVELOPER_*` and 6 `PLATFORM_DEVELOPER_*` keys at the top, ahead of
everything else: Apple names one directory three ways, and we emitted only the
plain `DEVELOPER_*` spelling. All twenty are that directory plus a fixed tail,
so they were 20 rows in the table the `DEVELOPER_*` keys already used rather
than 20 separate decisions. `-sdk macosx` went from 91 Apple-only to 71, and
matching from 393 to 413.

The tails are not derivable from the key names, which is the part worth
recording: `SYSTEM_DEVELOPER_APPS_DIR` and
`PLATFORM_DEVELOPER_APPLICATIONS_DIR` are both `/Applications`,
`SYSTEM_DEVELOPER_TOOLS` carries no `_DIR`, and `SYSTEM_DEVELOPER_DIR` is the
directory itself with no tail at all. A rule built from the name would get all
three wrong, so the table is spelled out.

Verifying this took a correction to the obvious test. The first attempt set
`DEVELOPER_DIR` to the staged tree and to CommandLineTools and compared the two,
and every one of the twenty came back byte-identical — because Apple ignores
`DEVELOPER_DIR` and reported its own path both times, so the two runs were not
running against different directories at all. `DEVELOPER_DIR` pointing at an
APFS clone of Apple's own developer directory, complete and valid, is still
ignored; `xcode-select -p` honours the same variable. So these twenty are the
*invoking binary's* directory plus a tail, which for this tool is the directory
it resolved, and the tails hold. The tests assert against a scratch directory
passed in as an argument, which a table of Apple's own paths cannot pass.

Earlier in the day, before the fixes below, the matched figures were
apple-only 93, ours-only 51, differing 33, matching 332. Progress:
| | before | after (project default) | after (`-sdk macosx`) |
| | --- | --- | --- |
| only in Apple | 93 | 67 | 71 |
| shared, differing | 33 | 6 | 0 |
| shared, matching | 332 | 385 | 413 |

The two columns differ only because of the missing SDK: the default build
cannot resolve one, so it is the weaker of the two measurements and should not
be read as parity.

The two outright bugs found earlier in this document were fixed first, which is
what the "419/52/49" figures below refer to.

The 419-line figure that earlier drafts of this file called "byte-identical to
Apple" is real, but it is **ours against the tree's** `xcodebuild` — the two
LibreDarwin tools agree exactly. It was never an Apple comparison, and reading
it as one is what hid this gap. Ours vs the tree re-verified today: 419 lines
each, byte-identical, same resolved `SDKROOT`, `DEVELOPER_DIR` and
`PLATFORM_DIR`.

#### Bugs found and fixed

The first two were found by the measurement above; the next two by chasing its
largest families down to a single cause. All four are fixed in the working tree,
and each has a regression test that fails against the commit before it.

| | was | now | why it mattered |
| --- | --- | --- | --- |
| default configuration | `Debug` | `Release` | with no `-configuration`, the same project built differently under the two tools — and worse, *our own* tool disagreed with itself: `-list` reported the project's `defaultConfigurationName` while the build silently used `Debug` |
| `SDK_VERSION` | `15.4` | `26.5` | the project hardcodes `SDKROOT` to `MacOSX.Internal.sdk`. We honoured `SDKROOT` but derived `SDK_DIR`/`SDK_NAME`/`SDK_VERSION` from the *default* scan (`/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk`), so the build compiled against one SDK while claiming another |
| source root | `.` | absolute | `SRCROOT` was `xc_dirname()` of the project argument, so a project named relatively — `xcodebuild -project Foo.xcodeproj`, which is how it is normally typed — left the root relative, and the 31 settings derived from it inherited that |
| intermediates | one level too high | follow `OBJROOT` | the whole build-directory chain was derived from a *seed* written before the project's settings were merged, so a project that moves its intermediates was overruled by our guess |

The configuration fix settles the default from the project's own
`defaultConfigurationName`, which is what `-list` was already reporting — one
answer now, read through one function (`project_default_configuration()`), with
`"Release"` left as the fallback for a project that names none. It has to
happen before the defaults load, because the name goes into
`BUILT_PRODUCTS_DIR`, the temporary directories and `CONFIGURATION`. The
`settings_load_defaults()` fallback was `"Debug"` and is now `"Release"` too, so
the two fallbacks cannot answer differently; that path is only reached for a
configuration-less load, but a fallback that contradicts the other one is the
same defect waiting for a caller.

The SDK fix adds `settings_sync_sdk_root()`, which re-reads the SDK that
`SDKROOT` names and takes its `Version` and `CanonicalName` from that SDK's own
`SDKSettings.plist` — via `read_sdk_info()`, the same read the defaults came
from, so an SDK in the older layout that answers from `info.ini` still answers
here and the two paths cannot diverge. It also derives
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

Writing the regression tests turned up a third case in the same family, which
the tests now cover. When `SDKROOT` names a directory that describes itself in
neither layout — not an SDK at all — Apple emits no `SDK_NAME`/`SDK_VERSION`, and
keeping the scan's defaults would name a *different* SDK, which is precisely the
defect above. The identity keys are now blanked in that case, with `SDK_DIR`
still following `SDKROOT` because that path is known whatever it is. This is the
one behaviour change here that is a visible output difference from before; it was
a choice between leaving the lie and losing the keys, and it is recorded because
someone reading `-showBuildSettings` for an unresolvable `SDKROOT` will notice
the empty values.

Verified after the fix: `SDK_DIR`/`SDK_NAME`/`SDK_NAMES`/`SDK_VERSION` and the
three numbered forms match Apple for the project default and for `SDKROOT=`
pointed at both `MacOSX26.5.sdk` and `MacOSX15.4.sdk`; an explicit
`SDK_VERSION=99.1` and `SDK_VERSION_ACTUAL=123` still win; `-list` is unchanged
against Apple; and the self-build fixed point still holds byte-identically.

##### The source root was not a path

`SRCROOT` came straight from `xc_dirname()` of the project argument, which is
right for `/abs/Foo.xcodeproj` and wrong for `Foo.xcodeproj`: the latter yields
`.`, and every `$(SRCROOT)/...` in the project then expanded to a relative path.
Every one of the 31 settings derived from it came out relative where Apple's are
absolute, and the emitted command lines inherited that.

The fix is `xc_abspath()` in `xcpath.c`, which resolves `.` and `..` *textually*
rather than through `realpath(3)`. That distinction matters: a build directory
that has not been created yet still needs a settled name, and a path that does
not exist must not fail the tool. Two attempts at the same function got it wrong
in instructive ways, and both are worth recording because the mistake is easy to
repeat:

- Writing the leading `/` before the components, then discarding a buffer left
  empty by a leading `..`, produced `Users/...` with no leading slash at all —
  a path that looks absolute in a diff and resolves as relative. Every one of
  31 settings changed at once, which is what made it obvious.
- Keying the leading `/` on whether the *input* was absolute was wrong for the
  common case, because the input here is `.` and the result is absolute because
  `getcwd` was prefixed. The flag has to describe the joined path.

The test that guards this has to name the project *relatively*. Passing the
absolute path — which is what every other test in the suite does — cannot see the
defect at all, because the old code was already correct there. That is worth
stating plainly: a test suite that only ever exercises the convenient form of its
input will pass against code that is broken in the normal form.

##### The intermediates were derived from a guess

The second half of the family is a different mistake with the same symptom. The
build directories are seeded before the project's settings are merged, because a
project writes its own in terms of them (`PRODUCT_NAME = $(TARGET_NAME)`) and
they have to expand to something. But they were then *left* at the seed: the seed
set `SYMROOT` and `OBJROOT` to the same directory and derived
`PROJECT_TEMP_DIR`, `CONFIGURATION_TEMP_DIR` and the target's own directory from
it, and the merge happened afterwards.

This project overrides both. Its `pbxproj` says
`OBJROOT = "$(SRCROOT)/build/obj/release"` and
`CONFIGURATION_BUILD_DIR = "$(SRCROOT)/build/release"`, so the seed was wrong on
both counts and the whole chain pointed one level too high, with a capitalised
`build/Release` where the project says `build/release`.

`derive_build_dirs()` now runs after the merge, reads the settled `OBJROOT` and
`CONFIGURATION_BUILD_DIR` back, and re-derives from them. Two details:

- The seed has to stay. It is what the merge expands against; removing it would
  break every project that refers to these keys.
- `CODESIGNING_FOLDER_PATH` and `DWARF_DSYM_FOLDER_PATH` are seeded in
  `build_apply_product_settings()`, which runs *before* the merge, so they had
  already latched the guess. They are refreshed in the same place, or they stay
  wrong while everything around them becomes right.

Together these four fixes took the matched comparison from 33 differing settings
to 11, and from 332 matching to 360.

##### Five settings the measurement named, and a sixth bug hiding behind one

The 11 that survived were chased to five keys and two defects, and the first
thing that had to be settled was which of the 11 were worth fixing at all.

Six of them exist only because `SDKROOT = MacOSX.Internal.sdk` names nothing.
Apple falls back to internal-SDK defaults when the SDK cannot be found, and we
report the toolchain's defaults instead. The way to tell the two groups apart is
to re-run the comparison with `-sdk macosx`, where the SDK resolves: the six
fallback keys agree there, and five do not. So five were ours:

- **`ARCHS_BASE` was `arm64 x86_64`, Apple says `$(ARCHS)`.** Not a host-architecture
  narrowing at all, as it first read — it is the standard list, spelled
  `ARCHS_BASE` in `CoreBuildSystem.xcspec`.
- **`TOOLCHAINS` was `MacOSX`, Apple says `com.apple.dt.toolchain.XcodeDefault`.**
  The identifier is written in `ToolchainInfo.plist`, and deriving it from the
  directory name is a guess that only ever happens to be right for the one
  toolchain Apple ships. `read_toolchain_info()` now reads it, which is the
  companion of the `read_sdk_info()` read the SDK fix already relied on.
- **`STRINGSDATA_DIR` was `Objects-normal`, Apple says `Objects-normal/undefined_arch`.**
  The `undefined_arch` component is Apple's name for "not the host's
  architecture", and it is not conditional on it: the value is the same for
  `ARCHS=arm64`, `ARCHS=x86_64`, and `ARCHS=arm64 x86_64`, so the suffix does not
  track `ARCHS` the way a reading of the name suggests.
- **`FRAMEWORK_SEARCH_PATHS` and `HEADER_SEARCH_PATHS`** were missing the
  `$(BUILT_PRODUCTS_DIR)` entries that `ENABLE_DEFAULT_HEADER_SEARCH_PATHS`
  adds. Both now prepend after the merge, and with the switch off they are the
  project's own values again.

`read_toolchain_info()` also had a return value that was wrong for the
toolchain it exists to describe. It reported failure whenever `info.ini` was
absent — and Apple's `XcodeDefault.xctoolchain` has no `info.ini`, only the
plist. Either file answering now counts as a read.

The sixth defect was not a setting at all. `-sdk` seeded the defaults and was
then overruled by the project's own `SDKROOT`, so against a project that
hardcodes `MacOSX.Internal.sdk` the build used the internal SDK while appearing
to ask for another — the same precedence bug as `SETTING=value`, one level up.
`-sdk` now outranks the merge, resolved to a path first so that the
name-to-path step downstream uses the SDK that was asked for rather than the one
the merge left behind.

With those, the project default's 6 differences are all Apple's fallback, and
`-sdk macosx` differs in nothing at all.

##### Fixed point

**The `178264` / `a42e08e8…` figure recorded earlier in this file was not
produced by a self-build, and has been withdrawn.** It was measured after a
build that named a tool which does not exist — the staged tree has no `xcodebuild`
at its root — and hashed a file the build had not written. The real figure,
produced by the procedure below, is `197624` bytes, sha256
`635e5ec974cfdf4249dc9f4f191daabed559a85745f2683f5ee6495fdefd692e`, identical
across two consecutive passes and identical to the source tree's own build.

Three things about that procedure are worth writing down, because each one
silently produces a plausible wrong answer rather than an error:

- The staged tool is at `build/release/Developer/usr/bin/xcodebuild`, not at
  `build/release/xcodebuild`. The tree root holds only `Developer/`, `Frameworks/`,
  `SharedFrameworks/` and `opt/`. A `build` that names the root path fails to
  launch, which is a loud failure; what is easy to miss is that the *build it
  does* can still be the previous one.
- Output overrides are command-line `SETTING=value` arguments, not environment
  variables. `CONFIGURATION_BUILD_DIR=/tmp/out` in the environment is silently
  ignored and the build lands in the project's own `build/release`, so a
  self-build that asks for a scratch directory can overwrite the tool in place
  and still exit 0. This is the `SETTING=` precedence rule from above, seen from
  the other side: the parser reads arguments only.
- The staged tool predates that fix, so it is still the one that ignores the
  override. That makes it a convenient demonstration of the bug and a bad choice
  of compiler for the fixed point.

So: point `DEVELOPER_DIR` at the staged `Developer`, build with the current
source-built tool, pass the output overrides as arguments, and hash the file the
build actually emitted:

```
DEVELOPER_DIR=<staged>/Developer ./build/release/xcodebuild \
    -project IDETools.xcodeproj -configuration Release \
    SYMROOT=$OUT OBJROOT=$OUT/Intermediates.noindex CONFIGURATION_BUILD_DIR=$OUT build
```

The earlier `3e19feaaefd15aa6` figure is the commit `v0.1.0`, and
`38adbd0e5f2f53a3ea0ad635b8ddc2371955f7a88f9b7dda7f92768b0cf7e1ee` was the
working tree with the four fixes above, at `197576` bytes. The `xcodebuild.c`
diff is ~190 lines of re-indent because the pbxproj had to be parsed before the
defaults load, which moved its one level of nesting; `-w` shows the real change
as 109 lines.

The rest of the gap is systematic, not incidental:

- **Paths are relative where Apple's are absolute** — `SRCROOT`/`PROJECT_DIR`
  `.` against `/Users/…/IDETools`, `BUILD_DIR` `./build` against an absolute
  path. Ours also uses `./build/Debug` where Apple uses
  `…/build/…/Release-iphoneos`-style derived directories.
- **71 settings missing outright** where the SDK resolves, including
  `ANDROID_DEPLOYMENT_TARGET`, `CCHROOT`, `LEGACY_DEVELOPER_DIR`,
  `DUMP_DEPENDENCIES_OUTPUT_PATH`, and a family of
  `*_DEPENDENCY_INFO_FILE` / `*_MAP_FILE_PATH` linker settings.
- **51 we emit that Apple does not**, mostly code-signing and Clang-warning
  defaults (`AD_HOC_CODE_SIGNING_ALLOWED`, `CLANG_ENABLE_MODULES`,
  `CLANG_WARN_*`).

The value-level differences that used to sit alongside these — `ARCHS_BASE`,
`TOOLCHAINS`, `STRINGSDATA_DIR`, the two search paths — are fixed above, and
with them the only value differences left anywhere are the six that are Apple's
fallback for a missing SDK.

So the honest status is: **behavioural parity is not met, and the gap is
measured rather than guessed.** Every value difference that survives measurement
is now accounted for: the six that remain on the project default are Apple's
fallback for an SDK that does not exist, and they agree with us once the SDK
does. What remains is coverage, not disagreement. Of Apple's 458 settings, 67
are absent from ours, and 6 carry a different value on the default build only —
before counting the 51 keys we emit that Apple never does. Certifying parity
means diffing `-showBuildSettings`, `-list` and the emitted compile/link
command lines against Apple's binary across a matrix of options and targets, and
closing the deltas above. Nothing in the fixed-point evidence substitutes for
this: a fixed point proves self-consistency, and ours agreeing with the tree's
tool only shows two implementations of the same `sdkpath.c` agree with each
other.

The next candidates, in the order they are worth doing, all come from the same
measurement rather than from reading code:

- **What an unset `DEVELOPER_DIR` means**, now that the measurement has turned
  this from a 10-key discrepancy into a 31-key one and shown it is a single
  decision rather than 31. Apple resolves the developer directory against the
  bundle it was launched from and ignores the variable entirely; `xcode-select`
  honours the variable. We honour it, and fall back to CommandLineTools. Every
  one of the 31 differences in the unmatched comparison above follows from that
  one choice, including the four `SDK*` keys, since `-sdk macosx` resolves
  inside whichever directory is in force. We keep honouring the variable —
  dropping it would break the staged self-build this repository is built with,
  and when it names Apple's own directory the two agree exactly, which is the
  0-differing comparison. What is left is the unset case: asking `xcode-select`
  before falling back to CommandLineTools would match Apple on any machine
  where the variable is not set, which is almost all of them.
- **The 71 settings we do not emit**, where the SDK resolves. The largest
  remaining group is per-arch and per-variant build directories derived from
  `OBJROOT` — `PER_ARCH_OBJECT_FILE_DIR`, `PER_VARIANT_OBJECT_FILE_DIR`,
  `GENERATED_MODULEMAP_DIR`, `SHARED_DERIVED_FILE_DIR`, the `REZ_*` and
  `*_FILE_LIST_*` paths — followed by the `$HOME`-relative install locations
  (`LOCAL_*`, `USER_*`) and a cache family (`CACHE_ROOT`, `CCHROOT`,
  `LD_MAP_FILE_PATH`, `COMPILATION_CACHE_CAS_PATH`, `SDK_STAT_CACHE_*`,
  `SDK_DIR_<name>`). Each of those three is worth measuring before implementing,
  since the first two turned out to be one rule each rather than the many
  separate decisions the count suggests.
- **The 51 we emit that Apple does not**, which need auditing rather than adding:
  each one is a claim about Apple's behaviour that has not been checked. On a
  build where the SDK resolves this is 29, and the 22 that disappear with the
  missing SDK are groups Apple's fallback suppresses wholesale rather than
  anything about ours — see the list above.
- **What Apple does about an unresolvable `SDKROOT`.** It does not error; it
  substitutes internal-SDK defaults. We report the toolchain's own defaults
  instead, which is defensible but is a difference in kind, not degree, and is
  the only place where the two tools answer a different question rather than
  reporting a different number. It is left undone on purpose: doing it by
  hardcoding the six values would re-break the resolvable case, which currently
  matches exactly.
