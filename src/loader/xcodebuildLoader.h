/*
 * xcodebuildLoader.h -- IDETools relaunch trampoline.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XCODE_TOOLS_XCODEBUILDLOADER_H_
#define _XCODE_TOOLS_XCODEBUILDLOADER_H_

#include <CoreFoundation/CoreFoundation.h>

/**
 * @var xcodebuildLoaderVersionNumber -- numeric loader version
 * @var xcodebuildLoaderVersionString -- printable loader version
 *
 * Emitted by Apple's versioned-dylib build step and part of the exported
 * surface, so the clean-room loader reproduces both names.
 */
extern double xcodebuildLoaderVersionNumber;
extern const unsigned char xcodebuildLoaderVersionString[];

/**
 * @func XcodeBuildSetInvocation -- record the tool's argument vector
 * @arg argc - argument count, as handed to main()
 * @arg argv - argument vector, as handed to main()
 *
 * The relaunch replays the tool's original arguments, but a dylib cannot
 * recover them: @c _NSGetArgc and @c _NSGetArgv still resolve on modern macOS
 * yet return garbage, and @c crashreporter.h is absent from MacOSX.Internal.sdk.
 * The tool therefore hands its own argv over before calling
 * @c LoadAddressSanitizerLibrariesIfPresentAndRelaunch.  Until this is called
 * the loader declines to relaunch rather than dropping the tool's arguments.
 */
void XcodeBuildSetInvocation(int argc, char * const *argv);

/**
 * @func LoadAddressSanitizerLibrariesIfPresentAndRelaunch
 * @arg force - relaunch even when the environment did not opt in
 * @arg commandName - expected basename of the host executable
 *
 * Re-executes the host tool with @c DYLD_IMAGE_SUFFIX=@c _asan so that dyld
 * resolves the @c _asan twins of our own dylibs, optionally inserting the Clang
 * AddressSanitizer runtime through @c DYLD_INSERT_LIBRARIES.  Returns without
 * doing anything when the host is not a tool we recognise, when the opt-in is
 * absent, or when the runtime cannot be located.  Does not return on success:
 * a successful relaunch replaces this process image via execv().
 *
 * @c commandName is a toll-free-bridged @c NSString in Apple's signature, so
 * @c CFStringRef is used here and the ABI is unchanged.
 */
void LoadAddressSanitizerLibrariesIfPresentAndRelaunch(Boolean force,
                                                       CFStringRef commandName);

/**
 * @func XcodeBuildMain
 * @arg allowASanRelaunch - whether the relaunch is permitted for this tool
 * @arg commandName - basename of the tool, e.g. @c xcodebuild
 * @arg bundleID - bundle identifier, used to key the environment snapshot
 * @arg logAspectName - name of the log aspect to report under
 * @arg timelineTraceFormat - format string for the timeline subactivity
 * @return true when the tool ran to completion
 *
 * Common entry point for IDETools command line tools: offers the
 * AddressSanitizer relaunch and records the default build settings.  Apple's
 * version goes on to drive @c Xcode3CommandLineBuildTool; see
 * @c xcodebuildLoader.c for what replaces that here.
 */
Boolean XcodeBuildMain(Boolean allowASanRelaunch, CFStringRef commandName,
                       CFStringRef bundleID, CFStringRef logAspectName,
                       CFStringRef timelineTraceFormat);

#endif /* _XCODE_TOOLS_XCODEBUILDLOADER_H_ */
