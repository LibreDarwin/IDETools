# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Open-source reimplementation of Apple's xcodebuild utility, matching the scope
# of Apple's IDETools project (PROJECT:IDETools-24902), which builds the real
# tool at <Xcode>/Contents/Developer/usr/bin/xcodebuild.  Apple's XCBuild
# project (PROJECT:xcbuild-24900.0.3) is a different thing and is not built
# here.
#
# The tool locates its own Developer directory from its own path and does its
# own SDK/toolchain discovery via the common/ sources.  That is deliberate:
# Apple splits the two roles, because /usr/bin/xcodebuild is only a ~500 byte
# xcselect shim (com.apple.dt.xcode_select.xtool-shim-public) that resolves the
# Developer dir and execs the real tool.  With no shim in this tree, the tool
# has to do that work itself.
#
# Build layout: every artifact lives under build/; the final tool goes to
# build/release/ or build/debug/ per CONFIG.
#
# Portable to both GNU make and BSD make (bmake): no pattern rules, no
# ifeq/ifdef/.if conditionals and no $(if)/$(shell) functions.  Per-config
# flags come from make/<CONFIG>.mk so both make variants behave identically.
#
# xcodebuild is also built by IDETools.xcodeproj; the two build systems agree
# on where objects and products land, so either one can be used from a clean
# tree without the other having run.

CONFIG ?= release
SDK    ?= /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.Internal.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

# PREFIX names an *absolute* Developer directory, so the tool lands at
# $(PREFIX)/usr/bin/xcodebuild.  That path is load-bearing, not cosmetic:
# devpath.c derives the Developer directory by stripping three path components
# from its own location, so a tool installed as $(PREFIX)/bin/xcodebuild would
# resolve the Developer directory to $(PREFIX)/.. and find no SDK or toolchain
# at all.  Install into usr/bin, never bin.
#
# PREFIX must be absolute so that DESTDIR staging composes correctly; there is
# no portable "current directory" variable (GNU make has CURDIR, bmake has
# .CURDIR) and conditionals are avoided in this Makefile, so the default is
# spelled out.  For in-tree staging without staging, use:
#     make install PREFIX=build/release/Developer
PREFIX  ?= /usr/local/Developer
DESTDIR ?=

# DESTDIR is concatenated verbatim, so it must carry its own trailing slash.
STAGEDIR = $(DESTDIR)$(PREFIX)

# The loader does not live inside the Developer directory.  Apple's layout is
# Contents/Frameworks/libxcodebuildLoader.dylib, a sibling of
# Contents/Developer, because the tool under Contents/Developer/usr/bin reaches
# it as ../../../Frameworks -- which is exactly the third run path Apple's
# xcodebuild carries.  Installing it as $(PREFIX)/Frameworks would instead give
# Contents/Developer/Frameworks, off that path, where nothing can load it.
# PREFIX names Developer, so Contents is its parent; override CONTENTS_DIR
# directly when staging under a real Xcode.app bundle.
CONTENTS_DIR     ?= $(PREFIX)/..
STAGED_CONTENTS  = $(DESTDIR)$(CONTENTS_DIR)

CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra \
	  -Wno-unused-parameter -I src/common -I src/xcodebuild

# The plist, settings and SDK code all talk to CoreFoundation; nothing else is
# linked.  xcodebuild resolves its Developer directory at runtime through
# devpath.c rather than a compiled-in prefix, so the build tree stays
# relocatable and needs no compiled-in prefix run path.
LFLAGS := -framework CoreFoundation

# The one run path the tool does carry is for the loader, which it dlopen()s as
# @rpath/libxcodebuildLoader.dylib rather than linking -- Apple's xcodebuild does
# the same and carries @executable_path/../../../Frameworks for it.  @loader_path
# comes first so the in-tree build finds the loader as a sibling, and the
# ../../../Frameworks entry covers a staged Contents/Developer/usr/bin layout.
XCODEBUILD_RPATH := -Wl,-rpath,@loader_path -Wl,-rpath,@executable_path/../../../Frameworks

# The loader additionally links Foundation, matching Apple's, which declares
# the same NSString-typed entry points.  Its version is pinned to 1.0.0 for the
# same reason: Apple's install name carries compatibility/current version 1.0.0
# and consumers check it.
LOADER_LFLAGS := $(LFLAGS) -framework Foundation
LOADER_VERSION := -compatibility_version 1.0.0 -current_version 1.0.0

XCODEBUILD := $(BUILD_DIR)/xcodebuild

# xcindex-test is the third IDETools product: the driver that exercises the
# build system APIs the index service talks to.  It shares the project model
# with xcodebuild (project.c) and the same path helpers, but does not link
# the loader and needs no Developer-directory discovery, so it carries only
# the objects it actually reaches.
XCINDEXTEST := $(BUILD_DIR)/xcindex-test

XCINDEXTEST_OBJS := $(OBJDIR)/xcindex-test.o $(OBJDIR)/project.o \
		    $(OBJDIR)/cfplist.o $(OBJDIR)/devpath.o $(OBJDIR)/sdkpath.o \
		    $(OBJDIR)/xcpath.o

# The loader is the second IDETools product: a dylib that owns the entry point
# so a tool can re-exec itself with DYLD_IMAGE_SUFFIX=_asan and have dyld bind
# the _asan twins of its own dylibs.  It is C with a CoreFoundation surface, and
# NSString arguments are CFStringRef by toll-free bridging, so the ABI is the
# one Apple's Objective-C entry points expose.
#
# The tool deliberately does not link it.  Apple's xcodebuild neither links nor
# imports the loader -- it is dlopen'd, and its run paths exist for
# DVTSystemPrerequisites -- so linking it here would make the tool refuse to
# start whenever the loader was not installed alongside it.
LOADER := $(BUILD_DIR)/libxcodebuildLoader.dylib
LOADER_OBJS := $(OBJDIR)/xcodebuildLoader.o

# Every object below is reached: the six xcodebuild/ files are the tool
# itself, and the four common/ files are the SDK/toolchain locator, the
# Developer-dir resolver, the property-list reader both of them consult, and
# a path helper.
# Reading a plist goes through CoreFoundation, which the tool already links,
# so there is no parser here.
XCODEBUILD_OBJS := $(OBJDIR)/xcodebuild.o $(OBJDIR)/build.o $(OBJDIR)/project.o \
		   $(OBJDIR)/settings.o $(OBJDIR)/ini.o $(OBJDIR)/index.o
COMMON_OBJS     := $(OBJDIR)/cfplist.o $(OBJDIR)/devpath.o $(OBJDIR)/sdkpath.o \
		   $(OBJDIR)/xcpath.o

XCODEBUILD_ALL_OBJS := $(XCODEBUILD_OBJS) $(COMMON_OBJS)

all: $(XCODEBUILD) $(LOADER) $(XCINDEXTEST)

$(XCODEBUILD): $(XCODEBUILD_ALL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(XCODEBUILD_ALL_OBJS) $(LFLAGS) $(XCODEBUILD_RPATH)

$(XCINDEXTEST): $(XCINDEXTEST_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(XCINDEXTEST_OBJS) $(LFLAGS)

$(LOADER): $(LOADER_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -dynamiclib -install_name @rpath/Frameworks/libxcodebuildLoader.dylib \
	  $(LOADER_VERSION) -o $@ $(LOADER_OBJS) $(LOADER_LFLAGS)

$(OBJDIR)/xcodebuildLoader.o: src/loader/xcodebuildLoader.c src/loader/xcodebuildLoader.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -I src/loader -c -o $@ src/loader/xcodebuildLoader.c

$(OBJDIR)/xcodebuild.o: src/xcodebuild/xcodebuild.c src/xcodebuild/xcodebuild.h \
                       src/common/cfplist.h src/common/devpath.h src/common/xcpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/xcodebuild.c

$(OBJDIR)/build.o: src/xcodebuild/build.c src/xcodebuild/xcodebuild.h \
                   src/xcodebuild/project.h src/common/xcpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/build.c

$(OBJDIR)/project.o: src/xcodebuild/project.c src/xcodebuild/project.h \
                     src/xcodebuild/xcodebuild.h src/common/sdkpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/project.c

$(OBJDIR)/settings.o: src/xcodebuild/settings.c src/xcodebuild/xcodebuild.h \
                      src/xcodebuild/ini.h src/common/sdkpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/settings.c

$(OBJDIR)/ini.o: src/xcodebuild/ini.c src/xcodebuild/ini.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/ini.c

$(OBJDIR)/index.o: src/xcodebuild/index.c src/xcodebuild/xcodebuild.h \
                    src/xcodebuild/project.h src/common/cfplist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/index.c

$(OBJDIR)/xcindex-test.o: src/xcindex-test/xcindex-test.c src/xcindex-test/xcindex-help.h \
                           src/xcodebuild/project.h src/common/xcpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcindex-test/xcindex-test.c

$(OBJDIR)/xcpath.o: src/common/xcpath.c src/common/xcpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/xcpath.c

$(OBJDIR)/cfplist.o: src/common/cfplist.c src/common/cfplist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/cfplist.c

$(OBJDIR)/devpath.o: src/common/devpath.c src/common/devpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/devpath.c

$(OBJDIR)/sdkpath.o: src/common/sdkpath.c src/common/cfplist.h src/common/sdkpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/sdkpath.c

install: all
	@case "$(DESTDIR)" in ""|*/) ;; *) \
	  echo "install: DESTDIR must end with '/', got '$(DESTDIR)'" >&2; exit 1 ;; \
	esac
	install -d $(STAGEDIR)/usr/bin
	install -d $(STAGED_CONTENTS)/Frameworks
	install -m 0755 $(XCODEBUILD) $(STAGEDIR)/usr/bin/xcodebuild
	install -m 0755 $(LOADER) $(STAGED_CONTENTS)/Frameworks/libxcodebuildLoader.dylib

# The tests drive the built tool and check it against the project's own pbxproj
# and the SDKs' own SDKSettings.plist, never against Apple's xcodebuild, so
# they need no Xcode.  TOOL is passed rather than assumed because the tool's
# path follows CONFIG.
test: all
	@TOOL=$(XCODEBUILD) sh tests/run.sh

# The parity tests are the opposite: they diff the reimplementation against
# Apple's own xcindex-test, so they need Xcode, and they are not part of
# `test`.  They are byte-for-byte, with only timings normalised away.
parity: all
	@MINE=$(XCINDEXTEST) sh tests/xcindex-parity.sh

# Likewise for -showBuildSettingsForIndex, which has a different oracle: Apple's
# xcodebuild rather than its xcindex-test, and whose settings the reimplementation
# reads itself rather than being handed.
forindex-parity: all
	@MINE=$(XCODEBUILD) sh tests/forindex-parity.sh

clean:
	rm -rf build

.PHONY: all install clean test parity
