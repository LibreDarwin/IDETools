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

CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra \
	  -Wno-unused-parameter -I src/common -I src/xcodebuild

# The plist, settings and SDK code all talk to CoreFoundation; nothing else is
# linked.  xcodebuild resolves its Developer directory at runtime through
# devpath.c rather than a compiled-in prefix, so the build tree stays
# relocatable and no rpath is needed here.
LFLAGS := -framework CoreFoundation

XCODEBUILD := $(BUILD_DIR)/xcodebuild

# Every object below is reached: the five xcodebuild/ files are the tool
# itself, and the five common/ files are the SDK/toolchain locator, the
# plist reader and the two alternative plist dialects it dispatches to.
XCODEBUILD_OBJS := $(OBJDIR)/xcodebuild.o $(OBJDIR)/build.o $(OBJDIR)/project.o \
		   $(OBJDIR)/settings.o $(OBJDIR)/ini.o
COMMON_OBJS     := $(OBJDIR)/devpath.o $(OBJDIR)/sdkpath.o $(OBJDIR)/plist.o \
		   $(OBJDIR)/xmlplist.o $(OBJDIR)/bplist.o

XCODEBUILD_ALL_OBJS := $(XCODEBUILD_OBJS) $(COMMON_OBJS)

all: $(XCODEBUILD)

$(XCODEBUILD): $(XCODEBUILD_ALL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(XCODEBUILD_ALL_OBJS) $(LFLAGS)

$(OBJDIR)/xcodebuild.o: src/xcodebuild/xcodebuild.c src/xcodebuild/xcodebuild.h \
                       src/common/devpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/xcodebuild.c

$(OBJDIR)/build.o: src/xcodebuild/build.c src/xcodebuild/xcodebuild.h \
                   src/xcodebuild/project.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/build.c

$(OBJDIR)/project.o: src/xcodebuild/project.c src/xcodebuild/project.h \
                     src/xcodebuild/xcodebuild.h src/common/plist.h \
                     src/common/sdkpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/project.c

$(OBJDIR)/settings.o: src/xcodebuild/settings.c src/xcodebuild/xcodebuild.h \
                      src/xcodebuild/ini.h src/common/plist.h \
                      src/common/sdkpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/settings.c

$(OBJDIR)/ini.o: src/xcodebuild/ini.c src/xcodebuild/ini.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/xcodebuild/ini.c

$(OBJDIR)/devpath.o: src/common/devpath.c src/common/devpath.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/devpath.c

$(OBJDIR)/sdkpath.o: src/common/sdkpath.c src/common/sdkpath.h src/common/plist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/sdkpath.c

$(OBJDIR)/plist.o: src/common/plist.c src/common/plist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/plist.c

$(OBJDIR)/xmlplist.o: src/common/xmlplist.c src/common/plist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/xmlplist.c

$(OBJDIR)/bplist.o: src/common/bplist.c src/common/plist.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/common/bplist.c

install: all
	@case "$(DESTDIR)" in ""|*/) ;; *) \
	  echo "install: DESTDIR must end with '/', got '$(DESTDIR)'" >&2; exit 1 ;; \
	esac
	install -d $(STAGEDIR)/usr/bin
	install -m 0755 $(XCODEBUILD) $(STAGEDIR)/usr/bin/xcodebuild

clean:
	rm -rf build

.PHONY: all install clean
