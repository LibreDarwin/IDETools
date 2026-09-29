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

### 3. `xcindex-test` — the console, reimplemented

383,584 bytes linking AppKit, libedit, Foundation, `DVTFoundation`,
`IDEFoundation` and a wide `libswift*` set including RegexBuilder and
StringProcessing. It is a test harness for the source index, and the index
itself depends on IDE frameworks we have no path to.

The index is out of reach; its command line is not. `src/xcindex-test/` is a C
reimplementation of the tool's front end — option parsing, the target-selection
algebra, the five deterministic commands (`help`, `beep`, `list-schemes`,
`list-indexables`, `print-stats`) and the libedit-style REPL — built on
`src/xcodebuild/project.c` for the project file and scheme reading. It matches
Apple byte for byte on all of that, timings aside; `make parity` diffs the two
live and reports 142 cases, all passing.

Two things about the tool's own command line are worth recording, because both
look like bugs and neither is. A `-project` with no value is not a missing
project: the option being present is what is required, and the open simply has
nothing to open, so the run succeeds against an empty workspace. And a path is
only a project if it ends in `.xcodeproj`; anything else — a directory, a path
to the pbxproj, a name that does not exist — opens nothing and is not an error
either, so the "file doesn't exist" message appears only for a path that claimed
to be a bundle. On a REPL line `--` ends the command rather than separating two,
so `list-schemes -- print-stats` fails on the `--` and not on the second action.

The seven engine-backed actions (`prepare`, `create-build-description`,
`print-build-description`, `print-build-description-targets`,
`print-destination`, `print-index-build-settings`, `index-files`) are a
different matter: their *work* is XCBuild, which this build does not have. What
is done is everything the console settles before the engine is reached — which
actions insist on an explicit target set, which resolve `-target` and the scheme
selectors before any per-action prerequisite (and which ignore a target they
cannot use), and the missing-prerequisite errors — and all of that is at parity
too. Where the engine would then do the work, the action stops with a message of
our own rather than an invented Apple one.

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
7. ◐ `xcindex-test` — the console and the selection algebra are implemented and
   at parity (`src/xcindex-test/`, `make parity`); the build actions are stubs
   that need XCBuild, so the engine half remains open.
8. ✅ Regression tests in `tests/run.sh`, run by `make test` and `bmake test`.
   70 assertions over the bugs above plus the unresolvable-`SDKROOT` case they
   turned up, each checked against the pbxproj, an SDK's own plist, a
   toolchain's own plist, or a rule read off Apple rather than against the tool
   itself. Against `ba8ef6c` the same file fails 17, and against the working
   tree at `8bcd2ce` it fails 13, so it discriminates on each of them.

   The count is also a function of the environment, which is worth knowing before
   reading a total as coverage. With `DEVELOPER_DIR` unset the suite reports
   180 passed and 4 skipped; pointed at Xcode it reports 197 and 0 skipped. The
   extra assertions are the SDK-loop and `TOOLCHAINS` families, which need an
   SDK and a toolchain whose bundle has a `ToolchainInfo.plist`, and
   CommandLineTools has neither the iOS platforms nor the toolchain plist. So
   the suite does not silently lose tests when run bare, but a run without
   `DEVELOPER_DIR` is short of a full one, and the skips are the only signal.
   Counts quoted elsewhere in this file are bare runs unless they say otherwise.

   `xcindex-test` is checked differently, because a test that agrees with a bug
   the two sides share proves nothing. `tests/xcindex-parity.sh` runs the
   reimplementation and Apple's own binary on the same input and diffs them byte
   for byte, with only the elapsed seconds normalised away — 142 cases over the
   fixtures in `tests/fixtures/`, driven by `make parity`. It needs Xcode, so it
   is deliberately not part of `make test`.

#### Status verdict — 2026-09-28

Re-assessed at `ba8ef6c` (this repo) and `82a443f` (tree), with the duplication
audit re-measured rather than carried forward:

| Scope | Verdict |
| --- | --- |
| Reachable products (1 and 2) | **complete** — built, verified, and self-building |
| Product 3, `xcindex-test` | **out of scope**, not unfinished — links IDE frameworks with no path to them |
| Behavioural parity with Apple | **not met, now measured** — with a `-sdk` that resolves, every platform agrees on every value except one: Apple also emits `SDKROOT` in its name form after the path form, and the name wins. All nine platforms measured (`macosx`, both iOS-family and both watch/vision-family pairs) hit that single key; without `-sdk`, the 6 that differ are Apple's fallback for a missing SDK; the bugs the measurement exposed are fixed |
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
settings emitted   apple 460, ours 491
  shared            440
  only in Apple      20
  only in ours       51
  shared, differing   6
```

(Count settings, not lines: raw line counts shift by a few depending on how the
trailing newline is handled, and a strict four-space regex silently drops keys.
The figures above come from matching `^\s*KEY = value` on either side; they
reconcile: 20 + 440 = 460 and 51 + 440 = 491, plus the 6 whose values differ,
which sit on the shared side.)

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
settings emitted   apple 486, ours 492
  shared            463
  only in Apple      23
  only in ours       29
  shared, differing   1
```

The single differing key is `SDKROOT`, and this one is not a fallback: Apple
emits it twice, once as the path its tool resolved and once as the platform
name form `macosx26.5`, and the last emission wins its dictionary. We emit the
path alone, so the two sides disagree about one key even though the value we
carry is exactly Apple's path form. Reconcile: 23 + 463 = 486, 29 + 463 = 492,
and the differing key is counted among the shared 463.

Zero differing keys except that one. The honest conclusion is that the six are
not defects in this tool but evidence that it does not reproduce a fallback for
a missing SDK, and hardcoding the fallback values would be a second way to be
wrong — it would break the resolvable-SDK case above, which currently matches.
What is left to do about the fallback itself is a separate question, deliberately
not answered here.

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

The same shape turned up again in the build directory, and this time the count
was wrong in the note that predicted it. The roadmap called the per-arch and
per-variant directories the largest remaining group; counted, they are six keys.
The largest is the twenty build-directory-rooted file lists, and it is large
because it is *not* a group — `FILE_LIST`, `LD_MAP_FILE_PATH`,
`PRECOMP_DESTINATION_DIR`, `REZ_COLLECTOR_DIR` and `PKGINFO_FILE_PATH` share a
prefix and no rule. What was one rule each is now done: five `OBJROOT` keys
(`COMPOSITE_SDK_DIRS`, `GENERATED_MODULEMAP_DIR`, `SHARED_PRECOMPS_DIR`,
`TEMP_SANDBOX_DIR`, `UNINSTALLED_PRODUCTS_DIR`) and two products-directory keys
(`SHARED_DERIVED_FILE_DIR`, `METAL_LIBRARY_OUTPUT_DIR`). As with the developer
family the names do not imply the locations: `SHARED_PRECOMPS_DIR` is nowhere
near `SHARED_anything`, and `COMPOSITE_SDK_DIRS` is plural where
`GENERATED_MODULEMAP_DIR` is singular. These are derived rather than seeded, so
they follow an `OBJROOT=` override — the tests assert that against a scratch
directory, because a seeded value would quietly point into a tree the caller is
not building.

Two things in this group were wrong on the first attempt, both from reasoning
about the name instead of asking.

`METAL_LIBRARY_OUTPUT_DIR` looked platform-specific, so it was gated on
`PLATFORM_NAME = macosx`. Apple emits it for macOS, iOS, tvOS and watchOS
alike — every platform installed here — so the gate would have suppressed the
key on three of them. It is emitted unconditionally now, and deliberately *not*
gated on `PLATFORM_NAME`, because that key does not track `-sdk` and reporting
macosx for an iOS target is exactly the kind of thing a gate should not be
built on. Its value is the products directory with a trailing slash and nothing
after it, which reads as a typo; Apple emits it that way and the tests assert
the exact string.

And the "fixed tail" rule had one exception. `GENERATED_MODULEMAP_DIR` is
`GeneratedModuleMaps` on the default platform and `GeneratedModuleMaps-iphoneos`
on another, while the other four are unsuffixed everywhere measured. That
exception was left unreproduced for a while, on the grounds that reproducing it
requires a platform name that tracks `-sdk` and ours does not — a suffix
derived from a `PLATFORM_NAME` stuck on macosx would be a suffix derived from a
lie, trading one wrong tail for two. The premise was right and the conclusion
wrong: the name is not only in `PLATFORM_NAME`. See the configuration-directory
section below, which reproduces the same suffix, and does it from the directory
the SDK was found in.

The next group is the ten install locations, and it is the first one where the
prefixes actively lie. `LOCAL_LIBRARY_DIR` is `/Library` while
`SYSTEM_LIBRARY_DIR` is `/System/Library` — the two prefixes name different
directories — yet `LOCAL_APPS_DIR` and `SYSTEM_APPS_DIR` are both
`/Applications` and both `_ADMIN_APPS_DIR` keys are both
`/Applications/Utilities`. No rule that maps one prefix onto the other can
satisfy that, so the table is spelled out and the tests pin both sides
absolutely. Pinning matters: "these two keys agree" also holds when both keys
are simply absent, which is exactly what happened when the first version of
these tests was run against a binary that had not actually been rebuilt.

Eight of the ten are fixed paths, identical on macOS, iOS, tvOS and watchOS, so
nothing here is platform-shaped. The other two are the trap in this group, and
the obvious source is the wrong one. `USER_APPS_DIR` and `USER_LIBRARY_DIR` hang
off the user's home, and the obvious way to spell "home" here is `getenv("HOME")`
— which is what the surrounding code does when it builds a child process
environment, and which would have been consistent with it. Apple does not do
that. Run Apple's `xcodebuild` with `HOME` pointing at an empty scratch
directory and it still reports the real home. The two keys come off
`getpwuid(getuid())->pw_dir`, the same source as the `HOME` setting that was
already matching. The tests override `HOME` and assert the keys stay under the
passwd home, and separately assert the override was visible to the environment,
so a `getenv`-based implementation fails rather than passing by accident.

#### Six caches, and a digest in one of their names

The six remaining `/var/folders` keys are two families, and neither is derivable
from a key name:

| key | value |
| --- | --- |
| `CACHE_ROOT`, `CCHROOT`, `SDK_STAT_CACHE_DIR` | `<user cache>/com.apple.DeveloperTools/<version>-<build>/<product>` |
| `COMPILATION_CACHE_CAS_PATH` | that directory + `/CompilationCache.noindex` |
| `CLANG_MODULES_BUILD_SESSION_FILE` | `<user cache>/org.llvm.clang/ModuleCache.noindex/Session.modulevalidation` |
| `SDK_STAT_CACHE_PATH` | that directory + `/SDKStatCaches.noindex/<name>-<build>-<md5>.sdkstatcache` |

`<user cache>` is `confstr(_CS_DARWIN_USER_CACHE_DIR)`, not `$HOME` and not
`NSTemporaryDirectory()`. The version is the *bundle's*, so the developer
directory has to be read back out to the `.app` that contains it: the path is
`<bundle>/Contents/version.plist`, and the two keys are `CFBundleShortVersionString`
and `ProductBuildVersion` — not `CFBundleVersion`, which is a different number
entirely (26.6-17F113 here, against 24959). `<product>` is the bundle's own name,
so a bundle called `FakeXcode.app` gives `.../FakeXcode`, not `Developer`.

A developer directory that is not inside a bundle — the CommandLineTools shape —
has no version to put in the path, so the four versioned keys are absent rather
than filled with a guess. The Clang one is not under the version directory and is
always reported.

The last component of the SDK stat cache name is an MD5 of the resolved SDK path,
which makes it the one place in the tool that computes a hash. It is computed
in-tree rather than through `CC_MD5`, which is deprecated and warns under this
project's flags, and the output is the lowercase hex of the little-endian digest
— the two ways of getting this wrong both produce a plausible 32-character
string rather than an error, and the byte-reversed one is exactly what
`snprintf("%08x%08x%08x%08x", h[0], h[1], h[2], h[3])` gives. The tests check it
against `/sbin/md5` on the SDK path the tool itself resolved, and the oracle for
the other two fields is the SDK's own `SystemVersion.plist`.

The name follows `-sdk`, so the caches are per-SDK; `CACHE_ROOT` does not. That
is the whole reason these six could be done before the platform model: nothing
here depends on a platform, only on which SDK is in force and which bundle
contains it.

#### The configuration directory is `Release-iphoneos`

Twenty-two of what remained were rooted at `SRCROOT`, and the largest part of
them turned out to hang off one directory whose name is not what it looks like.
A configuration's directory is `Release` on macOS and `Release-iphoneos`,
`Release-watchos`, `Release-iphonesimulator` on the others — the platform name
appended with a hyphen — so everything derived from it is wrong on a non-default
platform. In this project 29 keys carry that suffix in their value; before this
was fixed 15 of them were reported as differing and the rest were missing
outright, which is why `-sdk iphoneos` used to show 56 differing keys and now
shows 41.

The rule is the platform name, and the platform name is not only in
`PLATFORM_NAME`, which is where this document used to look and where it does not
live: `PLATFORM_NAME` reports `macosx` for every platform, so a suffix taken
from it is a suffix derived from a lie. It is also in the directory the SDK was
found in — `.../Platforms/<Name>.platform/Developer/SDKs/<sdk>` — and
lowercasing `<Name>` is the suffix exactly. `SDKROOT` already resolves correctly
on every platform measured, so the name is read from a value known to be right.

Verified against Apple for all nine SDKs, by name and by path, and the two
spellings are what rule out the shortcut. `-sdk iphoneos26.5` gets
`-iphoneos`, and so does `-sdk /…/iPhoneOS.platform/…/iPhoneOS26.5.sdk`, so
neither the version nor the path reaches the suffix. macOS is the exception
rather than the rule: `macosx`, `macosx26.5` and a path to the macOS SDK all get
no suffix at all, which is why the macosx measurement alone cannot tell a correct
implementation from one that always emits an empty suffix.

#### Sixteen keys under the target's own directory

With the base right, the rest of the `SRCROOT` group is one more family:
everything else under `$(CONFIGURATION_TEMP_DIR)/$(TARGET_NAME).build`. Sixteen
keys, and the paths are two rules rather than one, which is the whole subtlety:

| | key | value under the target directory |
| --- | --- | --- |
| no architecture | `FILE_LIST`, `PKGINFO_FILE_PATH`, `PRECOMP_DESTINATION_DIR`, `REZ_COLLECTOR_DIR`, `REZ_OBJECTS_DIR`, `PER_VARIANT_OBJECT_FILE_DIR` | a fixed tail |
| `CURRENT_ARCH` | `PER_ARCH_OBJECT_FILE_DIR`, `PER_ARCH_MODULE_FILE_DIR`, `PROCESSED_INFOPLIST_PATH`, `LD_DEPENDENCY_INFO_FILE`, `LD_MAP_FILE_PATH` | `Objects-normal/$(CURRENT_ARCH)/…` |
| one per arch in `ARCHS` | `LINK_FILE_LIST_normal_<arch>`, `LM_AUX_CONST_METADATA_LIST_PATH_normal_<arch>`, `SWIFT_RESPONSE_FILE_PATH_normal_<arch>` | `Objects-normal/<arch>/…` |

The second and third rows disagree about what an architecture is. The second
says `undefined_arch`, and says it whatever `ARCHS` is set to — verified across
`arm64`, `x86_64`, `arm64 x86_64`, `arm64 arm64e` and an empty list, because it
is a property of being asked for settings rather than for a build. The third
says `arm64`. An implementation that treats the two as one variable gets 15 of
these right on macOS and all of them wrong on the other platforms.

The third row is a per-architecture expansion, which is why a single-arch
project does not exercise it: `ARCHS="arm64 x86_64"` produces six keys, not
three, and an empty `ARCHS` produces none. The name is subscripted into the key
*and* used as a directory component, so the table in the code carries a stem and
an extension rather than a key.

Two of them carry the target's own name, which a path derived from the key name
gets backwards — the file is named after the product, not after the directory.
And one looks like the rest and is not: `PROJECT_DERIVED_FILE_DIR` hangs off
`$(OBJROOT)/$(PROJECT_NAME).build`, so it has neither the configuration nor the
target in its path where `DERIVED_FILE_DIR` has both. Apple keeps the two one
word apart.

### The nine that name the project and the target

None of the sixteen above is fixed text; all of them are paths, and every one of
them can be derived from something already in the table. The nine left in the
same family are fixed text in name only — each is a *translation* of something
else, and four of the nine have a plausible wrong answer that still looks like a
valid value.

`PROJECT` is the project's own name and `TARGETNAME` is the target's, which is
`TARGET_NAME` without the underscore. Those two are nearly free. The rest:

| key | rule | the wrong answer that looks right |
| --- | --- | --- |
| `PROJECT_GUID` | MD5 of the `.xcodeproj` filename, extension included | the directory, or the pbxproj's contents |
| `PACKAGE_TYPE` | from the target's `productType` | the `productType` itself |
| `STRIP_STYLE` | from the target's `productType` | one value for all targets |
| `VERSION_INFO_FILE` | `$(PRODUCT_NAME)_vers.c` | `$(TARGETNAME)_vers.c` |
| `VERSION_INFO_BUILDER` | the passwd entry's login name | `$USER` |
| `VERSION_INFO_STRING` | `"@(#)PROGRAM:…  PROJECT:…"` | one space, or `MARKETING_VERSION` |
| `XPCSERVICES_FOLDER_PATH` | one fixed path | — |

`PROJECT_GUID` is the one worth reading twice, because the value it is derived
from is not the one a first reading suggests. It is the digest of the project's
*filename*, extension and all — `md5("IDETools.xcodeproj")` is
`06c6d787d2b9f8e5eca60b744348fec5` for this project — and it does not depend on
the directory, nor on the pbxproj's contents. A renamed copy reports a different
GUID; the same file reached through a symlink reports the same one, which is why
the name is canonicalized with `realpath()` before it is digested. Taking the
basename of the path as typed gets this wrong in the one case that is easy to
hit: `xcodebuild -project /tmp/Alias.xcodeproj` then reports the digest of
`Alias.xcodeproj`. Both 32 characters, no error, wrong project identity.

`PACKAGE_TYPE` and `STRIP_STYLE` are read off the target's `productType`, and the
two values measured here are the two this project uses:

| `productType` | `PACKAGE_TYPE` | `STRIP_STYLE` |
| --- | --- | --- |
| `com.apple.product-type.tool` | `com.apple.package-type.mach-o-executable` | `all` |
| `com.apple.product-type.library.dynamic` | `com.apple.package-type.mach-o-dylib` | `debugging` |

A product type that is neither maps to nothing rather than to a guess. That is a
deliberate gap: two are measured, and the alternative — a default that is wrong
for every unmeasured type while looking right — is the same class of error this
work has been removing.

`VERSION_INFO_FILE` is named after the product, not the target, and the two
differ for the library here: `PRODUCT_NAME` is `xcodebuildLoader` while
`TARGETNAME` is `libxcodebuildLoader.dylib`, so the key is
`xcodebuildLoader_vers.c`. A path built from the target's name is right on the
tool target and wrong on the library, which is the useful shape for a test to
have.

`VERSION_INFO_BUILDER` is the login name from the password database — the same
name `id -un` prints — and it is *not* `$USER`. Overriding or unsetting `USER`
does not change it, so reading the environment would be right only for the
common case and silently wrong under a build system that sets `USER`.

`VERSION_INFO_STRING` is a version banner, and the two details that matter are
the double space after `PROGRAM:` and the hyphen. The version comes from
`CURRENT_PROJECT_VERSION`, not `MARKETING_VERSION`; the tool target sets no
`CURRENT_PROJECT_VERSION` and still gets a trailing hyphen rather than an empty
string, so `xcodebuild` reads `"@(#)PROGRAM:xcodebuild  PROJECT:IDETools-"` and
the library reads `…"PROJECT:IDETools-0.1.0"`.

All nine are defaults, so an output override still wins, and that was checked
against Apple for each of the seven that are derived rather than constant:
`PROJECT=Other`, `PROJECT_GUID=deadbeef`, `TARGETNAME=Nom`, `STRIP_STYLE=none`,
`PACKAGE_TYPE=x`, `VERSION_INFO_BUILDER=zz` and
`XPCSERVICES_FOLDER_PATH=/zz` are all honoured by both tools. `macOS` drops from
32 absent keys to 23, and from 452 matching to 461.

Nineteen assertions cover this, and 16 of them fail against the previous commit.
Three pass either way, and for honest reasons: one checks `PRODUCT_NAME`, which
already existed; the two override assertions show the override whether or not
anything is derived. Two more were vacuous on the first pass and were rewritten —
they compared the real project against a symlink to it, and against a second
target, so with the key absent from both sides the empty values matched. Both
now compare against the pinned literal, which is also why the GUID is written
into the test as a constant: an implementation that hashed the wrong thing would
still produce 32 well-formed hex characters, and a self-comparison would not have
noticed.

That exception was the visible edge of a much larger gap, and the larger gap
was the platform model. With `-sdk iphoneos` this tool used to emit
macOS-shaped settings: 41 differing keys, nearly all of them one omission —
there was no platform that tracked `-sdk`. `PLATFORM_NAME`, `PLATFORM_DISPLAY_NAME`,
`SUPPORTED_PLATFORMS`, `PLATFORM_PREFERRED_ARCH`, `SDK_NAMES` and
`SWIFT_PLATFORM_TARGET_PREFIX` were all the default platform's values, and
`ARCHS_STANDARD*` and `VALID_ARCHS` listed a Mac's architectures for an iOS
target. It reached past the platform keys too: the configuration directory was
`Release-iphoneos`, not `Release`, so every derived `*_TEMP_DIR` was wrong as
well. The platform model fixes all of it: the selected SDK names its platform,
and a table shaped per platform carries the platform keys and the per-SDK
suggested values. Every comparison quoted in this document is `-sdk macosx`
because that is the one resolvable SDK every machine here has, not because the
tool only knows macOS.

Earlier in the day, before the fixes below, the matched figures were
apple-only 93, ours-only 51, differing 33, matching 332. Progress:
| | before | after (project default) | after (resolvable `-sdk macosx`) |
| | --- | --- | --- |
| only in Apple | 93 | 67 | 23 |
| shared, differing | 33 | 6 | 1 |
| shared, matching | 332 | 385 | 462 |

The two columns differ only because of the missing SDK: the default build
cannot resolve one, so it is the weaker of the two measurements and should not
be read as parity. The middle column is also a snapshot rather than a live
figure — it was taken where the tool resolved no SDK at all, and it is not
re-run here, because in this tree the ambient toolchain selection resolves the
default SDK to a staged internal SDK under `../xcode-tools`, which is a
configuration and not a parity case. The resolvable-`-sdk` column is the one to
read. On `-sdk macosx` it hits 462 matching; the shape holds across every
platform — each of the nine measured reports exactly one differing key, the
`SDKROOT` double-emission, with the number of shared keys varying only with how
many extra keys Apple's per-SDK `Info.plist` carries (the shared total is 463
on macosx down to 441 on `xrsimulator`).

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
`-sdk macosx` differs only in Apple's `SDKROOT` name-form double-emission.

##### Fixed point

**The `178264` / `a42e08e8…` figure recorded earlier in this file was not
produced by a self-build, and has been withdrawn.** It was measured after a
build that named a tool which does not exist — the staged tree has no `xcodebuild`
at its root — and hashed a file the build had not written. The real figure,
produced by the procedure below, is `215192` bytes, sha256
`1fd0e277798323dd5f08ab05a1e1e3c7e9c3db32318a10a59f63460ec8585c1b`, and it is
byte-identical across two consecutive passes. It moves whenever the source does,
so it is a property of the tree rather than a fact about the tool; the figure
here was recomputed after the platform model, which is why it is neither the
`214760` that the previous commit recorded nor the `197704` before that. An
earlier version of this paragraph also claimed the result was identical to the
source tree's own `build/release` binary. That is not reproducible and is not
claimed: the tree build is `195176` bytes, because it is compiled with different
flags by a different driver. Only the two-pass equality is asserted.

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
- **23 settings missing outright** where the SDK resolves, including
  `ANDROID_DEPLOYMENT_TARGET`, `LEGACY_DEVELOPER_DIR`, `GCC_SYMBOLS_PRIVATE_EXTERN`
  and `REZ_EXECUTABLE`; `DUMP_DEPENDENCIES_OUTPUT_PATH` is no longer one of
  them, nor is `CCHROOT`, nor any of the nine `*_INSTALL_PATH` and
  `VERSION_INFO_*` values, nor `PROJECT_GUID`, nor `PACKAGE_TYPE`. What is left
  is `PATH`, `LOCROOT`, `WORKSPACE_DIR`, the two trailing-space search paths,
  and the two `LIBRARY_*_INSTALL_PATH` keys.
- **51 we emit that Apple does not**, mostly code-signing and Clang-warning
  defaults (`AD_HOC_CODE_SIGNING_ALLOWED`, `CLANG_ENABLE_MODULES`,
  `CLANG_WARN_*`).

The value-level differences that used to sit alongside these — `ARCHS_BASE`,
`TOOLCHAINS`, `STRINGSDATA_DIR`, the two search paths — are fixed above, and
with them the only value differences left are the six that are Apple's
fallback for a missing SDK, plus, on every resolvable `-sdk` build, the single
`SDKROOT` double-emission described in the parity section.

So the honest status is: **behavioural parity is not met, and the gap is
measured rather than guessed.** Every value difference that survives measurement
is now accounted for: the six that remain on the project default are Apple's
fallback for an SDK that does not exist, and they agree with us once the SDK
does. What remains is coverage, not disagreement. Where the SDK resolves, Apple
emits 486 settings of which 23 are absent from ours — before counting the 29
keys we emit that Apple never does — and the one differing shared value is the
`SDKROOT` double-emission. Certifying parity means diffing
`-showBuildSettings`, `-list` and the emitted compile/link command lines against
Apple's binary across a matrix of options and targets, and closing the deltas
above. Nothing in the fixed-point evidence substitutes for this: a fixed point
proves self-consistency, and ours agreeing with the tree's tool only shows two
implementations of the same `sdkpath.c` agree with each other.

The next candidates, in the order they are worth doing, all come from the same
measurement rather than from reading code:

- **What an unset `DEVELOPER_DIR` means.** I wrote this item up expecting the
  answer to be "ask `xcode-select` before falling back to CommandLineTools",
  and that expectation was wrong. Chasing it found the CLT answer was never the
  compiled-in fallback: our own `$HOME/.xcdev.dat` says
  `/Library/Developer/CommandLineTools` while `xcode-select -p` says the Xcode
  path. The two tools read different configuration files, and on this machine
  they disagree. Reordering the chain would not have changed the answer, so
  there was no fix to make — the two are each correct given their own config,
  which is why the matched comparison is the one worth reading.
- **Reject a derived directory that is not one.** The same investigation found a
  real defect. The fallback derives the developer directory by stripping three
  path components off the tool's own location and accepted whatever directory
  that named. The test harness lives at `build/release/xcodebuild`, so three
  components is the repository root: with no configuration present at all, an
  unconfigured build tree reported the repository as `DEVELOPER_DIR` and
  derived all 30 developer keys inside it, every one of them a path with nothing
  behind it — worse than the CommandLineTools default it displaced, because the
  paths look plausible. The derivation now requires `usr/bin`, which every real
  Developer directory has and the repository root does not. Fixed in the
  follow-up commit; the two new assertions fail without it.
- **The 23 settings we still do not emit**, where the SDK resolves. The note here
  used to call the per-arch and per-variant directories the largest group; counted,
  they are six keys. The largest used to be the twenty-two `SRCROOT`-rooted file
  lists, and it was large because it was *not* one rule — `FILE_LIST`,
  `LD_MAP_FILE_PATH`, `PRECOMP_DESTINATION_DIR`, `REZ_COLLECTOR_DIR` and
  `PKGINFO_FILE_PATH` shared a prefix and nothing else. Sixteen of them turned out
  to hang off the target's own directory and are now done, along with the six
  per-user caches, which took the largest group from twenty-two to six. The six
  groups that did turn out to be one rule each are all finished: the `OBJROOT`
  family (`COMPOSITE_SDK_DIRS`, `GENERATED_MODULEMAP_DIR`,
  `SHARED_PRECOMPS_DIR`, `TEMP_SANDBOX_DIR`, `UNINSTALLED_PRODUCTS_DIR`), the two
  products-directory keys (`SHARED_DERIVED_FILE_DIR`,
  `METAL_LIBRARY_OUTPUT_DIR`), the ten `SYSTEM_*`/`LOCAL_*`/`USER_*` install
  locations, the six caches, the sixteen under the target directory, and the nine
  that name the project and target. What is left is 23, and the measured shape of
  it is worth having rather than guessing at: 6 rooted at `SRCROOT`, 8 under the
  Developer directory (`PLATFORM_DIR`, `TOOLCHAIN_DIR`, the `SDK_DIR_*` pair, the
  two test search paths, `XCODE_APP_SUPPORT_DIR`, and `PATH`, which is a list of
  those paths rather than one of them), and 9 fixed values with no path in them at
  all. The `/var/folders` group that was in the original count is now empty. The
  `SRCROOT` group is down to the four project-rooted keys and the two
  trailing-space search paths, and it is the one worth attacking next. One
  correction to a claim this document used to make: two of the six *are* rooted at
  `CONFIGURATION_BUILD_DIR` — `LIBRARY_SEARCH_PATHS` and `REZ_SEARCH_PATHS`, both
  the products directory with a trailing space — so "none are
  build-directory-rooted" was wrong twice over, and the group is not purely
  project-rooted either.
- **The platform model, done in its first slice.** Everything above is macOS.
  With `-sdk iphoneos` this tool used to emit macOS-shaped settings: 41
  differing keys, nearly all of them tracing to one omission — there was no
  platform that tracked `-sdk`. The model is now in: the selected SDK names its
  platform, and a per-platform table carries `PLATFORM_NAME`,
  `PLATFORM_DISPLAY_NAME`, `PLATFORM_FAMILY_NAME`, `SUPPORTED_PLATFORMS`,
  `PLATFORM_PREFERRED_ARCH`, `SDK_NAMES`, `SWIFT_PLATFORM_TARGET_PREFIX`,
  `LLVM_TARGET_TRIPLE_OS_VERSION`, the `ARCHS_STANDARD*` / `VALID_ARCHS`
  values, the `PLATFORM_DEVELOPER_*_DIR` family, the `BUNDLE_*_FOLDER_PATH`
  family, the deployment-target default and its `DEPLOYMENT_TARGET_SUGGESTED_VALUES`,
  and `ENTITLEMENTS_DESTINATION` (which itself is per SDK name, not platform).
  Every platform measured — `macosx`, `iphoneos`, `iphonesimulator`,
  `appletvos`, `appletvsimulator`, `watchos`, `watchsimulator`, `xros`,
  `xrsimulator` — now reports exactly one differing key, the `SDKROOT`
  double-emission (462 matching on macosx, down to 440 on `xrsimulator` as the
  shared total shrinks), and the `-<platform>`-suffix, `Release-<platform>`
  configuration directory and `*_TEMP_DIR` families fall out of `PLATFORM_NAME`.
  What remains in this space is the 23-settings item above, which is now a
  question of unpicking the path groups Apple carries in its per-SDK
  `Info.plist` rather than a question of platform shape.
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
