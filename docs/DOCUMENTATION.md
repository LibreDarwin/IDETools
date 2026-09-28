# IDETools — Investigation Findings

Working notes for the `xcodebuild` reimplementation. Everything below was
verified locally on macOS (Xcode 26.4 / build 2660) unless explicitly marked
as inference.

Related: `docs/IDETools.md` (project rules, toolchain and SDK paths).

> **Naming.** This repo was renamed from `XCBuild` to `IDETools` in `86ee59b`.
> Apple's real `xcodebuild` belongs to the **IDETools** project
> (`PROJECT:IDETools-24902`), not to XCBuild (`PROJECT:xcbuild-24900.0.3`).
> Apple's XCBuild / SwiftBuild work is deferred; see §7.

---

## 0. How IDETools and xcselect share code — they don't

This is the question that motivated the rename, and the answer is that
**there is no shared code**. The two never link against each other; they are
coupled only by a process boundary.

`/usr/bin/xcodebuild` is not the tool. It is a shim:

```
bundle id:  com.apple.dt.xcode_select.xtool-shim-public
            (embedded Info.plist: xtool-shim-public, DTXcode 2630,
             DTSDKName macosx26.5.internal)
arch:       fat, x86_64 + arm64e
size:       135,488 bytes
__text:     0x1ec = 492 bytes   (arm64e slice)
__cstring:  6 bytes
symbols:    22 total, of which 12 are undefined imports
```

Its **complete** undefined-symbol list — i.e. everything it needs from outside:

```
_error  _stack_chk_fail  _stack_chk_guard  __NSConcreteGlobalBlock
_NSGetExecutablePath  __NSGetProgname  _dispatch_once  _getattrlist
_strdup  _strrchr
_xcselect_invoke_xcrun          <-- the only xcselect call, by a wide margin
```

So the shim is 492 bytes that:

1. `__NSGetProgname` — learns its own name (`xcodebuild`).
2. `_NSGetExecutablePath` + `_strrchr` + `_strdup` — learns its own path.
3. `_getattrlist` — checks it really is a developer tool.
4. `_xcselect_invoke_xcrun` — hands the whole problem to `libxcselect`.

`libxcselect` resolves the Developer directory and the real executable, and the
shim `exec`s it. The 135 KB of the fat binary is not logic — it is an embedded
PropertyList template (the shim builds a plist to describe the request it hands
to `xcselect_invoke_xcrun`, which is why the binary contains the full PLIST DTD
and every `<key>BuildServiceName</key>`-style tag) plus a small plist writer.

### What this means for us

The real tool therefore **never needs libxcselect**. It only ever runs *inside*
an already-selected Developer directory, so it derives that directory from its
own location. Our `src/common/devpath.c` does precisely this and is a faithful
mirror of the technique:

```c
_NSGetExecutablePath(buf, &size);
realpath(buf, real);
for (i = 0; i < 3; i++)          /* strip  xcodebuild / bin / usr  */
        p = strrchr(real, '/');
```

`<Xcode>/Contents/Developer/usr/bin/xcodebuild`, minus three components, is
`<Xcode>/Contents/Developer` — the Developer directory.

So there are two legitimate designs, and they are mutually exclusive:

| | Apple | This tree (current) |
| --- | --- | --- |
| Who picks the Developer dir | shim, via `libxcselect` | the tool, via its own path |
| Does the tool link libxcselect | no | no |
| Where SDK/toolchain lookup lives | tool + libxcselect (duplicated on Apple's side) | `src/common/sdkpath.c` |
| Is `src/common` needed | no | yes |

`src/common` is a **substitute for the shim + libxcselect architecture**, not a
port of shared code. If we later add a shim (see §7), `devpath.c` becomes
redundant; if we do not, `src/common` is correct where it is and should stay in
*this* repo rather than move to the `xcselect` repo.

> **Correction.** An earlier revision of this document claimed all of
> `src/common` "belongs in libxcselect". That is wrong. `libxcselect` answers
> *which* Developer directory is selected; `devpath.c` + `sdkpath.c` are the
> tool's own discovery. Only a shim would belong to that layer.

---

## 1. Executive summary

Apple's XCBuild is **not** a from-scratch build system. It is a thin Apple-side
shim layer wrapped around **SwiftBuild**, which is open source at
<https://github.com/swiftlang/swift-build>.

The modern layering, outermost first:

```
/usr/bin/xcodebuild                     shim  -> libxcselect + libSystem   (22 symbols)
  -> <Xcode>/Developer/usr/bin/xcodebuild   real tool (IDETools; ObjC/Swift)
       -> XCBuild.framework                52 KB reexport shim
            -> SwiftBuild.framework         3.4 MB  the actual engine
                 -> XCBBuildService.bundle   52 KB legacy service entry point (PlugIn)
```

Consequences for this project:

- We should **not** reimplement a build engine. Consume `swift-build`.
- Our work is **IDETools**: the `xcodebuild` tool, plus (later, if wanted) the
  `XCBuild.framework` reexport shim.
- IDETools is exactly three products — `xcodebuild`, `xcindex-test`,
  `libxcodebuildLoader.dylib`. See §2.1.
- `swift-build` is **Apache-2.0 with Runtime Library Exception**, so it stays a
  separately-licensed dependency. Only our own files are BSD-3.

---

## 2. Which Apple binary belongs to which project

Read from the `PROJECT:` marker each binary carries. This is the single most
useful table in this document.

| Binary | `PROJECT:` marker | Project |
| --- | --- | --- |
| `/usr/bin/xcodebuild` | *(none — shim)* | xcselect tooling |
| `<Xcode>/Developer/usr/bin/xcodebuild` | **`IDETools-24902`** | **IDETools** |
| `<Xcode>/Developer/usr/bin/xcindex-test` | **`IDETools-24902`** | **IDETools** |
| `Contents/Frameworks/libxcodebuildLoader.dylib` | **`IDETools-24902`** | **IDETools** |
| `XCBuild.framework/Versions/A/XCBuild` | `xcbuild-24900.0.3` | XCBuild |
| `SwiftBuild.framework` | `xcbuild-24900.0.3` | XCBuild |
| `XCBBuildService.bundle` | `xcbuild-24900.0.3` | XCBuild |
| `SharedFrameworks/DVTSystemPrerequisites.framework` | *(none)* | not IDETools — see §3.2 |

So Apple's `xcbuild` project contains exactly three products, and the
`xcodebuild` tool is **not** one of them.

### 2.1 IDETools contains exactly three products

Found by scanning every executable in `Developer/usr/bin`, `SharedFrameworks`,
`Frameworks` and `PlugIns` for the `PROJECT:IDETools-` marker. Exactly three
hits:

| Product | Size | Role |
| --- | --- | --- |
| `Developer/usr/bin/xcodebuild` | 100,528 | the tool — what this repo builds |
| `Developer/usr/bin/xcindex-test` | 383,584 | index test harness |
| `Contents/Frameworks/libxcodebuildLoader.dylib` | 73,440 | loader / relaunch trampoline |

`xcindex-test` links AppKit, libedit, Foundation, `DVTFoundation`,
`IDEFoundation` and a wide set of `libswift*` (incl. RegexBuilder and
StringProcessing). It is an IDE-adjacent tool, not build-system code, and
depends on frameworks we do not have.

`libxcodebuildLoader.dylib` is the interesting one and the most plausible next
piece of IDETools to build — it is small and self-contained:

```
install name:  @rpath/Frameworks/libxcodebuildLoader.dylib
links:         @rpath/Xcode3Core.framework/Versions/A/Xcode3Core
               @rpath/DVTFoundation.framework/Versions/A/DVTFoundation
               Foundation, libobjc.A, libSystem.B
exports:       _XcodeBuildMain
               _LoadAddressSanitizerLibrariesIfPresentAndRelaunch
               _xcodebuildLoaderVersionNumber
               _xcodebuildLoaderVersionString
```

It holds the real entry point as `_XcodeBuildMain` precisely so the tool can
**relaunch itself under AddressSanitizer** on request. Note it lives in
`Contents/Frameworks/`, not `Developer/`, and is linked `@rpath`.

### 2.2 Apple's DeveloperTools is ~33 separate projects

Every `PROJECT:` marker found in `Developer/usr/bin` — one repo per project, so
LibreDarwin's `DeveloperTools/` should mirror that split:

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

Only `IDETools` is this repo. `gnumake` and `libtrace` are the notable
low-hanging neighbours, being small and self-contained.

---

## 3. Apple's binary architecture (verified)

### 3.1 `/usr/bin/xcodebuild` — the shim

See §0. Links `libxcselect.dylib` (compat/current 1.0.0) and `libSystem.B.dylib`
(compat 1.0.0, current 1356.0.0) and nothing else.

Note `/Applications/Xcode.app/Resources` does **not** exist on this machine; the
real path is under `Contents/`.

### 3.2 `<Xcode>/Contents/Developer/usr/bin/xcodebuild` — the real tool (IDETools)

```
size:  100,528 bytes
links: @rpath/DVTSystemPrerequisites.framework/Versions/A/DVTSystemPrerequisites
       /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
       /usr/lib/libobjc.A.dylib
       /usr/lib/libSystem.B.dylib
       /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
```

No `XCBuild.framework` and no `libxcselect` in `LC_LOAD_DYLIB`. It does import
`_dlopen` / `_dlsym`, so parts of it are loaded at runtime, but there are no
`xcselect` or `DEVELOPER_DIR` strings in the binary — consistent with §0: the
tool does not do Developer-dir selection.

`DVTSystemPrerequisites.framework` is **not** an IDETools product: it carries no
`PROJECT:` marker at all, lives in `SharedFrameworks/`, is 176,512 bytes of
Swift, links PackageKit, and exports `DVTFirstLaunchPackagesVersionInfo` types
(first-launch package / licence handling). It is a dependency we would have to
reimplement or stub if we ever wanted byte-identity; it is not ours to build.

### 3.3 `XCBuild.framework` — reexport shim

```
path:   /Applications/Xcode.app/Contents/SharedFrameworks/XCBuild.framework
size:   Versions/A/XCBuild = 52,544 bytes
links:  @rpath/XCBuild.framework/Versions/A/XCBuild                    (install name)
        @rpath/SwiftBuild.framework/Versions/A/SwiftBuild            (current 24900.0.3, **reexport**)
        Foundation (weak), libobjc, libSystem
        libswiftCoreFoundation / Darwin / Dispatch / IOKit /
        ObjectiveC / XPC / os   (all weak)
```

**It exports exactly two symbols:**

```
_XCBuildVersionNumber
_XCBuildVersionString
```

and zero `XCBuild`-module Swift symbols. It is a literal empty umbrella: a
reexport shim and nothing else, plausibly ~50 lines. `Versions/A` contains only
`XCBuild`, `PlugIns`, `Resources`, `_CodeSignature`.

The `reexport` flag is the load-bearing detail — `XCBuild` is a
compatibility/umbrella framework whose entire job is to re-export `SwiftBuild`.

> **Correction.** An earlier note claimed `XCBuild.framework` contains nested
> `SWBProjectModel.framework`, `SWBProtocol.framework` and `SWBUtil.framework`.
> That is **wrong** — no such nested frameworks exist. The engine ships as one
> flat `SwiftBuild.framework`. Verified with
> `find .../XCBuild.framework/Versions/A -maxdepth 1` and a filesystem-wide
> search for `SWB*`.

### 3.4 `SwiftBuild.framework` — the engine

```
path:   /Applications/Xcode.app/Contents/SharedFrameworks/SwiftBuild.framework
id:     com.apple.dt.SwiftBuild
version: CFBundleShortVersionString 16.0, CFBundleVersion 24900.0.3
built:  DTXcode 2660, DTSDKName macosx26.4.internal
size:   3,413,216 bytes
exports: 3,597 symbols, Swift module named **XCBuild**, not SwiftBuild:
           _$s7XCBuild0A10GetVersionSSyKF
           _$s7XCBuild11SWBBuildQoSO10backgroundyA2CmFWC
           _$s7XCBuild11SWBUserInfoV13homeDirectorySSvg
```

Note the module/bundle mismatch: bundle `SwiftBuild.framework`, Swift module
`XCBuild`, public API prefixed `SWB*`. Any Swift shim must
`@_exported import XCBuild`, not `import SwiftBuild`.

### 3.5 `XCBBuildService.bundle` — legacy service entry point

```
path:   .../XCBuild.framework/Versions/A/PlugIns/XCBBuildService.bundle/Contents/MacOS/XCBBuildService
id:     com.apple.dt.XCBBuildService
size:   51,728 bytes
links:  Foundation, libobjc, libSystem   (no engine framework -> loaded at runtime)
```

**Yes — `XCBBuildService` is part of Apple's internal `xcbuild` project.** The
`PROJECT:xcbuild-24900.0.3` marker is decisive, and it is the *same* project and
build version that produces `SwiftBuild.framework`. Apple's internal `xcbuild`
repo holds both the shim layer and the SwiftBuild engine sources; the open-source
`swiftlang/swift-build` is the public mirror of the engine, where the same service
is named `SWBBuildServiceBundle` instead.

### 3.6 `libxcselect` — Developer-dir / SDK resolution

Apple's `libxcselect` was extracted from the dyld shared cache with
`ipsw dyld extract`. Its 12 public exports are exactly:

```
xcselect_bundle_is_developer_tool           xcselect_get_manpaths
xcselect_developer_dir_matches_path         xcselect_get_version
xcselect_find_developer_contents_from_path  xcselect_host_sdk_path
xcselect_get_developer_dir_path             xcselect_invoke_xcrun
                                            xcselect_manpaths_free
                                            xcselect_manpaths_get_num_paths
                                            xcselect_manpaths_get_path
                                            xcselect_trigger_install_request
```

Internal symbols include `_sdks_at_path`, `_get_developer_dir_from_symlink`,
`_xcselect_host_sdk_path`, and it carries the string
`%s/Platforms/MacOSX.platform/Developer/SDKs` — so it does enumerate SDKs, for
`xcrun`'s benefit. That duplicates the tool's own SDK lookup, on Apple's side
too.

Our built `libxcselect.dylib` already exports the identical 12 names. The
`xt_*` functions in `src/common` are **not** among them and are not exported;
they are the tool's own discovery API, not libxcselect's (see §0).

---

## 4. `swiftlang/swift-build` (upstream, deferred)

- <https://github.com/swiftlang/swift-build> — 2.2k stars, 2,165 commits.
- **License: Apache License v2.0 with Runtime Library Exception.** Not BSD-3, and
  not relicensable as such.
- "A high-level build system based on llbuild ... used by SwiftPM, Xcode, and
  Swift Playground."
- Swift tools 6.2, C++20, platforms macOS 15+ / iOS 18+ / macCatalyst 18+.

### 4.1 Products

| Product | Kind | Notes |
| --- | --- | --- |
| `swbuild` | executable | CLI |
| `SWBBuildServiceBundle` | executable | the build service; the process name to attach a debugger to |
| `SwiftBuild` | library | corresponds to `SwiftBuild.framework` (module `XCBuild`) |
| `SWBBuildService` | library | |
| `SWBProjectModel` | library | |
| `SWBProtocol` | library | |
| `SWBUtil` | library | |
| `MockToolchainCASPlugin` | dynamic lib | test only |

Platform targets: `SWBApplePlatform`, `SWBUniversalPlatform`,
`SWBGenericUnixPlatform`, `SWBAndroidPlatform`, `SWBQNXPlatform`,
`SWBWebAssemblyPlatform`, `SWBWindowsPlatform`.

Dependencies: `swift-driver`, `swift-system`, `swift-argument-parser`,
`swift-tools-protocols`, `swift-llbuild`.

### 4.2 Upstream workflows worth reusing

- `swift package --disable-sandbox run-xcodebuild` — runs `xcodebuild` from the
  `xcode-select`ed Xcode configured to use a locally built build service.
- `swift package --disable-sandbox launch-xcode` — same, for the GUI.
- `SWIFTCI_USE_LOCAL_DEPS=1` — switches dependencies to sibling paths
  (`../swift-driver`, `../swift-system`, `../swift-argument-parser`,
  `../swift-tools-protocols`, `../llbuild`). **This is the pattern to copy** for
  referencing sibling repos instead of vendoring or submoduling them.
- Env switches: `SWIFTBUILD_STATIC_LINK`, `SWIFTCI_USE_LOCAL_DEPS`,
  `SWIFTBUILD_LLBUILD_FWK`.
- Debugging: attach to process named `SWBBuildServiceBundle`.

---

## 5. Current state of the LibreDarwin tree

Root: `/Users/sunneva/xnuports-root/devel/xcode-tools`

Source layout is `<category>/<repo>`:

```
src/apple/…   src/apple-internals/…   src/cctools-helpers/   src/extras/
src/git/      src/golang/             src/lib/               src/misc/
src/openxc-tools/   src/other/         src/puredarwin/        src/python/
src/qemu/     src/remorix/            src/swiftlang-llvm/
```

Relevant checkouts:

```
src/DeveloperTools/XCBuild            <- this repo (being renamed to IDETools)
src/openxc-tools/xcodebuild          xcodebuild.c, build.c, ini.c, project.c, settings.c
src/openxc-tools/libxcselect         libxcselect.c, xcselect.h
src/swiftlang-llvm/swift-build       the engine
```

### 5.1 Already built and installed (`build/release/`)

| Artifact | Built from | Notes |
| --- | --- | --- |
| `Developer/usr/lib/libxcselect.dylib` | `src/openxc-tools/libxcselect` | 54,544 bytes; still exactly 12 `xcselect_*` exports, install name `@rpath/libxcselect.dylib` |
| `Developer/usr/bin/xcodebuild` | `src/openxc-tools/xcodebuild` | 188,120 bytes (rebuilt after the CoreFoundation backport) |
| `SharedFrameworks/SwiftBuild.framework` | `src/swiftlang-llvm/swift-build` | `com.apple.dt.SwiftBuild`, 16.0, `24900.0.3`, DTXcode 2660, `macosx26.5` |
| `XCBuild.framework` | — | **absent — the gap, deferred** |

`build/release/SharedFrameworks/` also has `BuildServerProtocol.framework`,
`LanguageServerProtocol.framework`, `LanguageServerProtocolTransport.framework`,
`llbuild.framework`, `SKLogging.framework`, `ToolsProtocolsSwiftExtensions.framework`
— the `swift-tools-protocols` products upstream already builds.

> Byte sizes here are a snapshot of whatever was last installed, and they drift
> with the tree. `xcodebuild` was 190,184 bytes before the CoreFoundation
> backport and 188,120 after; `libxcselect` moved 36,352 → 54,544 for reasons
> unrelated to this repo, with its export surface unchanged at 12. Re-measure
> rather than trusting either number.

### 5.2 The LibreDarwin `SwiftBuild.framework` matches Apple's

| | Apple | LibreDarwin |
| --- | --- | --- |
| `CFBundleVersion` | 24900.0.3 | 24900.0.3 |
| `CFBundleShortVersionString` | 16.0 | 16.0 |
| `DTXcode` | 2660 | 2660 |
| `DTSDKName` | macosx26.4.**internal** | macosx**26.5** |
| `XCBuild`-module symbols | 3,597 | 3,594 |

Same source, same build version — so linking the LibreDarwin-built framework
reproduces Apple's layering, and `XCBuild.framework` becomes a thin reexport
wrapper we could build ourselves.

### 5.3 The tree already builds our exact sources

The tree's build system wires in `src/openxc-tools/` through `.include`
fragments, so our repo's two source directories are not merely similar to what
ships — they are the same code, already compiled into the tree:

```
mk/tool.d/xcodebuild.mk   T_SRCS = build.c ini.c project.c settings.c xcodebuild.c
                          T_SRCS += src/openxc-tools/common/xcpath.c
                          .include "${TOP}/mk/with-devpath.mk"
                          .include "${TOP}/mk/with-sdkpath.mk"
                          T_LDADD += -framework CoreFoundation

mk/with-devpath.mk        T_SRCS += src/openxc-tools/common/devpath.c
                          T_CFLAGS += -I${TOP}/src/openxc-tools/common
mk/with-sdkpath.mk        T_SRCS += src/openxc-tools/common/sdkpath.c
                          T_SRCS += src/openxc-tools/common/cfplist.c
```

There is **no `mk/with-plist.mk`** in the tree any more, and the hand-rolled
parsers it used to pull in are gone from both sides. That is the end state, not
a TODO: the tree's `sdkpath.c` used to call `plist_parse_any()`, so the
fragment existed only in the tree; once `sdkpath.c` was unified on the
CoreFoundation reader, both sides dropped the parsers and the fragment had
nothing left to add. `-framework CoreFoundation` was already on the link line
here, so the parsers bought nothing but maintenance.

The fragment structure still mirrors our `Makefile` one-for-one, including the
`-framework CoreFoundation` choice and the same comment about `.pbxproj` being
an OpenStep plist. Duplication status, re-measured on both trees at
`ba8ef6c`/`82a443f` on 2026-09-28 — every cell below was checked with `cmp`, not
carried forward from the previous draft:

| Path | Status |
| --- | --- |
| `src/xcodebuild/{ini,project,settings}.{c,h}`, `xcodebuild.h` | byte-identical |
| `src/xcodebuild/build.c` | ours ahead — `is_cxx()` + the `OTHER_CPLUSPLUSFLAGS` branch |
| `src/xcodebuild/xcodebuild.c` | ours ahead — the loader hand-off (see `docs/IDETools.md`) |
| `src/common/{devpath,sdkpath,cfplist,xcpath}.{c,h}` | byte-identical, all four |
| `src/common/{bplist,plist,xmlplist}.{c,h}` | **gone from both sides** — see §6.2 |
| `src/common/` extra in tree | `json.*`, `wrapper.*`, `xcresult.*` |

`diff` reports `build.c` and `xcodebuild.c` as 62 and 83 changed lines, but that
is mostly re-indentation churn — those two files are indented deeper in the
tree. Normalising leading whitespace gives the real figure:

| File | Only in ours | Only in the tree | What it actually is |
| --- | --- | --- | --- |
| `build.c` | 18 | 2 | `is_cxx()` plus the `OTHER_CPLUSPLUSFLAGS` branch that uses it |
| `xcodebuild.c` | 63 | 4 | `<dlfcn.h>`, the 54-line loader hand-off, the 7-line ASan relaunch guard; the tree keeps one extra `-showBuildSettings` help line |

Both remaining differences are **deliberate, one-directional**: the two
`xcodebuild.c` blocks are the loader product, which is ours alone — Apple's and
the tree's tool `dlopen` nothing. The `build.c` block is the C/C++ distinction
behind `OTHER_CPLUSPLUSFLAGS`, which the tree does not wire because it has no
C++ sources to distinguish. Neither is a pending backport.

`sdkpath.c` was the one file that genuinely had a direction of travel: it was
+91/−17 with `xt_sdk_build_version()` (which reads an SDK's `SystemVersion.plist`,
since an SDK does not name its own build) and the tree's copy predated it. That
is resolved — both sides are now the same `sdkpath.c`, `cfplist.c` and
`xcpath.c`, and `exportOptions.plist` in the tree's `xcodebuild.c` reads through
the same CoreFoundation helper rather than an ad-hoc XML scraper. The tree's
`json.*` / `wrapper.*` / `xcresult.*` belong to other tools (`xcresulttool` and
friends) and are still not ours to pull in.

> Nothing in this section is a pending backport. For the scope verdict that
> follows from it — both reachable products complete, and behavioural parity
> with Apple measured and **not met**, after the two outright bugs the
> measurement exposed were fixed — see `docs/IDETools.md` §Plan.

### 5.4 There is no shim in the LibreDarwin tree

`Developer/usr/bin/xcodebuild` is the real tool, not a shim:

| | Apple (real tool) | LibreDarwin |
| --- | --- | --- |
| size | 100,528 | 188,120 |
| links | Foundation, CoreFoundation, libobjc, `DVTSystemPrerequisites` | CoreFoundation, libSystem |
| `PROJECT:` marker | `IDETools-24902` | none |
| does its own Developer-dir lookup | no (shim does) | **yes** — `src/common/devpath.c` |

So the "byte-identical to Apple's" rule in `docs/IDETools.md` is not currently
met, and is not reachable without `DVTSystemPrerequisites` plus the
Objective-C/Swift tool body. It is reachable for a *shim*, which is a ~500-byte
program.

---

## 6. `src/common` — what is actually used

Measured, not assumed. `src/xcodebuild/` reads every property list through
CoreFoundation, and now shares the reader in `src/common/cfplist.c` rather than
having one of its own:

| Name that looks local | Reality |
| --- | --- |
| `plist_set_str` (`build.c:1482`) | static fn over `CFMutableDictionaryRef`/`CFStringRef` — **CoreFoundation** |
| `settings_merge_plist_dict` (`settings.c:1061`) | takes `CFTypeRef` — **CoreFoundation** |
| `xmlplist_get` | **gone** — it was an ad-hoc XML scraper in `xcodebuild.c` that only shared a name; `exportOptions.plist` now goes through `cfplist_read()` |

> A substring grep is misleading in the other direction now: `plist_get`
> matches inside **`cfplist_get`**, which *is* a call into `src/common`. Look
> for the `cfplist_` prefix instead. (`plist_dict` still matches only inside
> `plist_dict_get`, and `plist_wanted` only inside `infoplist_wanted`; neither
> reaches `src/common`.)

### 6.1 What remains is four files

`src/common` is `devpath.c`, `sdkpath.c`, `cfplist.c` and `xcpath.c`. All four
are byte-identical in both trees and all four are wired into the tree's build:

```
devpath.c   -> xt_default_developer_dir()                (2 call sites)
sdkpath.c   -> xt_find_sdk, xt_find_toolchain, xt_first_sdk_name,
               xt_foreach_sdk, xt_sdk_setting, xt_default_developer_dir
cfplist.c   -> cfplist_read, cfplist_get, cfplist_string   (1 + 3 call sites)
xcpath.c    -> xc_dirname()                               (3 call sites)
```

`cfplist.c` and `xcpath.c` are the two that the tree did not have. `cfplist.c`
(126 lines) is the plist reader extracted out of `sdkpath.c` so that
`xcodebuild.c` could use it too — three of its call sites read
`exportOptions.plist`. `xcpath.c` (43 lines) is the `dirname(2)` wrapper, and
its three call sites are exactly the ones that broke on a bare project name:
`build.c:3950` (`SRCROOT`), `xcodebuild.c:504` (project root) and
`xcodebuild.c:801` (workspace root).

### 6.2 The hand-rolled plist parser is gone

`plist.c`, `xmlplist.c`, `bplist.c` and `plist.h` — **1,246 lines**, about 61% of
the old `src/common` — are gone from **both** trees. On our side they were
reachable from exactly one place:

```
sdkpath.c  read_plist()  ->  plist_parse_any()
```

reached solely via the four `xt_*` string readers in `sdkpath.c`
(`xt_sdk_setting`, `xt_platform_setting`, `xt_toolchain_identifier`,
`xt_sdk_default_property`, plus `xt_sdk_build_version`, which reads
`SystemVersion.plist`). On the tree side `sdkpath.c` called `plist_parse_any()`
too, which is the only reason `mk/with-plist.mk` ever existed there.

The replacement is `cfplist.c`: `cfplist_read()` returns a `CFDictionaryRef`
from `CFPropertyListCreateWithData()`, `cfplist_get()` pulls one member out and
`cfplist_string()` hands it back as a `strdup`'d C string. CoreFoundation was
already linked (`LFLAGS := -framework CoreFoundation`), so the parsers bought
nothing but maintenance. Extracting it into `src/common/` is what let
`xcodebuild.c` reach it as well — the reader now has two consumers, ours and
the tree's, and the tree's ad-hoc XML scraper for `exportOptions.plist` is gone
with it.

Three details worth keeping:

- **`CFSTR()` cannot take a variable.** It expands to `"" cStr ""`, so it needs
  a literal. `cfplist_get()` therefore builds the lookup key with
  `CFStringCreateWithCString()`. Using `CFSTR(key)` on a `char *` is a compile
  error, not a runtime one.
- **One dialect check instead of two parsers.** `CFPropertyListCreateWithData`
  sniffs the format, which is what the hand-rolled `plist_looks_like_xml()` /
  `plist_looks_like_binary()` pair existed to do. CoreFoundation also *rejects* a
  malformed file, which the hand-rolled scanner did not — a `bplist00` with
  trailing garbage used to be accepted.
- **The root must be a dictionary, and that has to be checked.** A property
  list can be rooted at an array or a string. Casting the parse result straight
  to `CFDictionaryRef` and looking a key up in it does not return NULL — it
  raises `NSInvalidArgumentException` and aborts the process. `cfplist_read()`
  therefore refuses any root that is not a dictionary, which is also what the
  deleted `plist_dict_get()` used to do.

Verified against the reader: XML and `bplist00` `SDKSettings.plist` give
identical `DisplayName` and `CanonicalName`; array-rooted, string-rooted, junk,
missing, empty, directory and malformed-binary inputs all return NULL.

The export path was checked separately during the backport with six
`exportOptions.plist` inputs (XML, bplist, no `method`, missing path, junk, and
a `method` that is a dict not a string), and ours and the tree's behaved
identically — XML and bplist agreeing on every key.

**Re-measured live**, once the clang blocker was cleared: the missing-clang exit
at `build.c:3109` was never about the export code. It is pre-existing, comes
from the Developer directory defaulting to CommandLineTools, and disappears with
`DEVELOPER_DIR` pointed at a tree that has a toolchain — the same environment the
rest of this comparison is measured in. With that set, the six inputs run through
the installed CLI on both tools:

| Input | Ours | Apple |
| --- | --- | --- |
| XML `method` | plist read, export proceeds | plist read, `exportArchive The archive contains nothing that can be signed.` |
| bplist `method` | identical to XML | identical to XML |
| no `method` | `rc=1`, `does not specify a 'method'` | `rc=65`, `archive not found at path …` |
| missing path | `rc=1`, `cannot read export options plist` | `rc=70`, `Couldn't load -exportOptionsPlist … No such file` |
| `method` is a dict | `rc=1`, `does not specify a 'method'` | `rc=70`, `Failed to decode "method". Expected to decode String but found a dictionary instead.` |

The plist reading is what is being compared, and it agrees on every input
including the malformed ones: both reject a dict-typed `method` and a missing
file, and both accept XML and bplist interchangeably. The rows where the exit
codes and messages differ are **not a parsing difference** — they are how far
each tool then gets. Apple proceeds to sign and export the archive and so
reports on the archive's contents; `do_export_archive()` in `xcodebuild.c`
validates and reads the options, prints the method, destination and team under
`-verbose`, and returns. It does not sign, package or upload, so on a valid
options plist it has nothing left to fail on where Apple has a real export to
attempt. Adding export is a feature, not a parity fix, and is out of scope here;
what is established is that the plist layer underneath it matches.

`devpath.c` is 55 lines for a single function; `xcpath.c` is 43.

### 6.4 Setting precedence in `-showBuildSettings`

The one ordering rule that everything else hangs off: **the command line is the
last word on any setting, which means last in the input, not last thing to
happen.** A value the user typed has to be in place before anything reads it
back to derive another value. Two bugs came from getting that wrong, in
opposite directions, and both were invisible until measured against Apple.

- **Deriving after the merge, before the overrides.** The build-directory chain
  is derived from `OBJROOT` and `CONFIGURATION_BUILD_DIR` after the project's own
  settings are merged in, which is correct — but the `SETTING=value` overrides
  were applied after *that*. So `CONFIGURATION_BUILD_DIR=/tmp/ovr` showed the
  new value in the key the user typed and the old value everywhere derived from
  it: `BUILT_PRODUCTS_DIR`, the search paths, the signing and dSYM folders. The
  tool agreed with itself in the wrong direction. Overrides are now applied
  before `derive_build_dirs()` as well as after, so both the key and everything
  derived from it report the value that was asked for.
- **`-sdk` overruled by the project.** `-sdk` used to seed the defaults and then
  be overruled by the project's `SDKROOT`, the same defect one level up. Against
  a project that hardcodes `SDKROOT = MacOSX.Internal.sdk`, `-sdk macosx` built
  against the internal SDK while appearing to ask for another. `-sdk` now
  outranks the merge. It is resolved to a path *first*, because the step that
  turns a bare SDK name into a path does so using whichever `SDK_DIR` the merge
  left behind — which is the SDK `-sdk` was meant to replace.

The asymmetry between those two is the point. `-sdk` and `SETTING=value` are
both the command line, so both win, but they land in different places: `-sdk`
has to be converted to a path before it can be stored, and the conversion needs
a developer directory, while `SETTING=value` carries its own value. Resolving
`-sdk` up front is what lets the same downstream name-to-path step serve both.

Both are tested against Apple's binary, and the second is what takes the
project's comparison from 4 differing settings to 0. The remaining 6 on the
default build are Apple's own fallback for an SDK that does not exist, not a
precedence problem; `IDETools.md` records why reproducing them was deliberately
not attempted.

---

## 7. Deferred: XCBuild / SwiftBuild

Parked until IDETools stands on its own. When it is picked up:

| Repo | Owns | Installs |
| --- | --- | --- |
| `DeveloperTools/IDETools` | the `xcodebuild` tool; later `XCBuild.framework` | `Developer/usr/bin/xcodebuild` |
| `openxc-tools/xcodebuild` | superseded by the above | — |
| `openxc-tools/libxcselect` | Developer-dir / SDK resolution for the shim and `xcrun` | `Developer/usr/lib/libxcselect.dylib` |
| `swiftlang-llvm/swift-build` | the engine (upstream, Apache-2.0) | `SharedFrameworks/SwiftBuild.framework` |

**No submodule.** `swift-build` already exists as a sibling checkout and is
already built and installed at the matching `24900.0.3`. If a local rebuild is
needed, recurse into the sibling path (the `SWIFTCI_USE_LOCAL_DEPS=1` pattern),
rather than vendoring a second copy — LibreDarwin already has
`src/swiftlang-llvm/swift-build`, so a submodule would duplicate it immediately.

Linking shape to reproduce, from `otool -L` on Apple's `XCBuild`:

```
-F <prefix>/SharedFrameworks
-framework SwiftBuild            # @rpath/SwiftBuild.framework/Versions/A/SwiftBuild
-rpath @loader_path/../Frameworks
```

### 7.1 Could the whole thing be an Apache-2.0 XCBuild repo?

Structurally yes, and cheaply — the Apple-specific surface is a 2-symbol
reexport framework plus a service shim, on top of swift-build. The fork/sibling
choice is the real decision, and the LibreDarwin layout already settles it in
favour of *no fork* (§7).

Licensing, if XCBuild is ever revived: swift-build is Apache-2.0 **with Runtime
Library Exception** (the LLVM patent carve-out), so a combined work states the
RLE, not plain Apache-2.0. This repo's own `LICENSE` is BSD 3-Clause and the
`xcselect` repo is BSD-3, so this is a real open question, not a detail.

### 7.2 Open decisions

1. **Add a shim, or keep the tool self-sufficient?** Keeping it self-sufficient
   means `src/common` stays; adding an `xtool-shim-public` clone would make
   `devpath.c` redundant and put Developer-dir selection back in `libxcselect`
   where Apple has it. This is the main open design question.
2. **Replace the plist trio with CFPropertyList?** ~1,246 lines, and the tool
   already links CoreFoundation.
3. **Byte-identity scope** — shim layer only, or the tool too (§5.4).
4. **Licence** — keep IDETools BSD-3, or move to Apache-2.0 (§7.1).
5. **Rename the GitHub remote** `LibreDarwin/XCBuild` -> `LibreDarwin/IDETools`
   to match `86ee59b`.

---

## 8. Build system in this repo (current)

Committed as `1d7e1d6` (sources + build system) and `86ee59b` (rename).

```
Makefile                  portable GNU make + BSD bmake
make/release.mk           release optimisation flags
make/debug.mk             debug optimisation flags
IDETools.xcodeproj/       project.pbxproj, single `xcodebuild` target
src/xcodebuild/           the tool
src/common/               devpath.{c,h} sdkpath.{c,h} cfplist.{c,h} xcpath.{c,h}
tests/run.sh              regression tests, POSIX sh, no Xcode required
```

`make test` (or `bmake test`) builds and then runs `tests/run.sh`, which drives
the built tool through 70 assertions covering the bugs this comparison found.
Each expectation is derived from something other than the tool
under test — the project's own `project.pbxproj`, an SDK's own
`SDKSettings.plist`, a toolchain's own `ToolchainInfo.plist`, or a rule read off
Apple's output — so none of them can pass by agreeing with a shared bug, and
none of them need Apple's binary to run. The
SDK cases are skipped when the machine has no SDKs; the project fixtures are
copies of the real `IDETools.xcodeproj` with `defaultConfigurationName`
rewritten, because a hand-written pbxproj is a second thing to get wrong. Run
against the pre-fix commit `ba8ef6c` the same file reports 17 failures, and
against `8bcd2ce` 13, so it discriminates rather than merely passing.

What got added after the rename, all on `master`:

| Commit | Change |
| --- | --- |
| `70f52aa` | `libxcodebuildLoader.dylib`, the second IDETools product |
| `6f452b7`, `f5c5508`, `41c4576` | plist parsing moved to CoreFoundation; `cfplist.{c,h}` extracted so `xcodebuild.c` can share it; `exportOptions.plist` read the same way |
| `aef42b2` | `xcpath.{c,h}`; a bare `IDETools.xcodeproj` resolves `SRCROOT` to `.` |
| `3e8d4b9` | build settings stored as arrays are expanded during the merge |
| `ba8ef6c` | `OTHER_CFLAGS`, `OTHER_CPLUSPLUSFLAGS` and `OTHER_LDFLAGS` honoured |

> The `XCBuildConfiguration` `isa` entries in `project.pbxproj`, and the
> `XCBuildConfiguration` mentions in `src/xcodebuild/project.h` and
> `src/xcodebuild/settings.c`, are **Xcode project-format class names** and
> must never be renamed to `IDETools`. Only the `PBXProject "XCBuild"` title
> was project identity and did change.

Constraints observed in the `Makefile`: no pattern rules, no
`ifeq`/`ifdef`/`.if`, no `$(if)`, no `$(shell)`, no `$(CURDIR)`.

Defaults:

```
CC      = <XcodeDefault.xctoolchain>/usr/bin/clang
SDK     = .../MacOSX.platform/Developer/SDKs/MacOSX.Internal.sdk
CONFIG  = release
product = build/$(CONFIG)/xcodebuild
objects = build/$(CONFIG)/obj/*.o
LFLAGS  = -framework CoreFoundation
PREFIX  = /usr/local/Developer        (an absolute *Developer directory*)
DESTDIR =                             (must carry its own trailing slash)
```

### 8.1 The install path is load-bearing

`PREFIX` names a Developer directory and the tool installs to
`$(PREFIX)/usr/bin/xcodebuild` — **never** `$(PREFIX)/bin/xcodebuild`. This is
not cosmetic: `devpath.c` derives the Developer directory by stripping three
path components from its own location, so a tool at `<P>/bin/xcodebuild` resolves
the Developer directory to `<P>/..` and finds no SDK or toolchain at all. An
earlier revision of this Makefile installed to `$(PREFIX)/bin` with
`PREFIX ?= /usr/local`, which is exactly that broken shape.

`PREFIX` must be absolute so `DESTDIR` staging composes correctly. There is no
portable current-directory variable — GNU make has `CURDIR`, bmake has `.CURDIR`,
and they are different variables — and this Makefile avoids conditionals, so the
default is spelled out rather than derived. `DESTDIR` is concatenated verbatim
and therefore must end in `/`; the recipe rejects it loudly instead of silently
producing a mangled path:

```
make install DESTDIR=/tmp/stage/                    # ok
make install PREFIX=build/release/Developer         # in-tree, no staging
make install DESTDIR=/tmp/stage                     # Error: must end with '/'
```

Verified working after the rename:

```
make                                     bmake
make CONFIG=debug                        bmake CONFIG=debug
xcodebuild -project IDETools.xcodeproj -configuration Release build
xcodebuild -project IDETools.xcodeproj -configuration Debug build
make install DESTDIR=/tmp/idetest/      -> .../Developer/usr/bin/xcodebuild
bmake install PREFIX=build/release/Developer
build/release/xcodebuild -version        ->  xcodebuild 0.1.0
otool -L build/release/xcodebuild        ->  CoreFoundation + libSystem only
```

The Xcode project's `SDKROOT` must be the full `MacOSX.Internal.sdk` path; the
platform-directory form fails with `error: unable to find sdk …`.

---

## 9. Reproducing these findings

```sh
# Which project owns which binary
for f in /usr/bin/xcodebuild \
         /Applications/Xcode.app/Contents/Developer/usr/bin/xcodebuild \
         /Applications/Xcode.app/Contents/SharedFrameworks/XCBuild.framework/Versions/A/XCBuild \
         /Applications/Xcode.app/Contents/SharedFrameworks/SwiftBuild.framework/Versions/A/SwiftBuild; do
  strings -a "$f" | grep -o 'PROJECT:[a-zA-Z]*-[0-9.]*' | sort -u
done

# The shim: 492 bytes, one xcselect call
nm -u /usr/bin/xcodebuild
otool -l -arch arm64e /usr/bin/xcodebuild | grep -A2 'sectname __text'
strings -a /usr/bin/xcodebuild | grep -i xtool

# The umbrella is empty
nm -gU /Applications/Xcode.app/Contents/SharedFrameworks/XCBuild.framework/Versions/A/XCBuild

# No nested SWB frameworks (see the correction in 3.3)
find /Applications/Xcode.app/Contents/SharedFrameworks/XCBuild.framework/Versions/A -maxdepth 1

# LibreDarwin tree
otool -L …/build/release/Developer/usr/bin/xcodebuild
find …/build/release -name 'XCBuild.framework'      # -> nothing; that is the gap

# src/common reachability
grep -rn 'plist_parse' src/            # expect: no hits
grep -c 'CFPropertyList' $SDK/System/Library/Frameworks/CoreFoundation.framework/Headers/CFPropertyList.h

# The three IDETools products: scan for the marker (expect exactly 3 hits)
for d in Contents/Developer/usr/bin Contents/SharedFrameworks Contents/Frameworks Contents/PlugIns; do
  find /Applications/Xcode.app/$d -maxdepth 4 -type f -perm -u+x 2>/dev/null | while read -r f; do
    strings -a "$f" 2>/dev/null | grep -q 'PROJECT:IDETools-' && echo "$f"
  done
done

# The loader trampoline
otool -D    /Applications/Xcode.app/Contents/Frameworks/libxcodebuildLoader.dylib
nm -gU      /Applications/Xcode.app/Contents/Frameworks/libxcodebuildLoader.dylib

# Who owns what: every PROJECT marker in Developer/usr/bin
for f in /Applications/Xcode.app/Contents/Developer/usr/bin/*; do
  [ -f "$f" ] || continue
  m=$(strings -a "$f" 2>/dev/null | grep -o 'PROJECT:[a-zA-Z]*-[0-9.]*' | sort -u | tr '\n' ' ')
  [ -n "$m" ] && printf '%-34s %s\n' "$(basename $f)" "$m"
done

# Duplication status against the tree
diff -rq /Users/sunneva/xnuports-root/devel/xcode-tools/src/openxc-tools/common src/common
diff -rq /Users/sunneva/xnuports-root/devel/xcode-tools/src/openxc-tools/xcodebuild src/xcodebuild

# ...but diff -rq overstates build.c / xcodebuild.c: those differ mainly by
# indentation. This is the recipe behind the table in §5.3.
python3 - <<'PY'
import difflib, re
T = "/Users/sunneva/xnuports-root/devel/xcode-tools/src/openxc-tools/xcodebuild"
I = "src/xcodebuild"
norm = lambda p: [re.sub(r'^\s+', '', l.rstrip())
                  for l in open(p, errors="replace") if l.strip()]
for f in ("build.c", "xcodebuild.c"):
    a, b = norm(f"{I}/{f}"), norm(f"{T}/{f}")
    d = [l for l in difflib.unified_diff(a, b, lineterm="", n=0)
         if l[:1] in "+-" and l[:3] not in ("+++", "---")]
    print(f, "ours:", len([x for x in d if x[0] == '-']),
          "tree:", len([x for x in d if x[0] == '+']))
PY

# The tree's build fragments that already compile these sources
cat /Users/sunneva/xnuports-root/devel/xcode-tools/mk/tool.d/xcodebuild.mk
cat /Users/sunneva/xnuports-root/devel/xcode-tools/mk/with-{devpath,sdkpath}.mk
```

Per `docs/IDETools.md`, the reverse-engineering toolchain is available for
deeper work: `ipsw` and friends in `…/build/release/opt/bin`, `jtool2` and
`disarm` in `/Users/sunneva/opt/re/bin`, and Binary Ninja via `bn`.

