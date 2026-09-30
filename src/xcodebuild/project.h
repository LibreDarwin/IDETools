/* xcodebuild -- open source reimplementation of Apple's xcodebuild utility
 *
 * Project / workspace introspection: locating project.pbxproj, listing
 * targets, configurations and schemes, scanning available SDKs and
 * toolchains, and extracting a target's XCBuildConfiguration.buildSettings.
 *
 * Copyright (c) 2026 Sunneva N. Mariu <sunnevanattsol@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __PROJECT_H__
#define __PROJECT_H__

#include <CoreFoundation/CoreFoundation.h>

#include "xcodebuild.h"
/* Resolve <path>/project.pbxproj for a .xcodeproj / path. Returns a malloc'd
 * absolute path (caller frees), or NULL if not found. */
char *project_pbxproj_path(const char *project);

/* Load and parse a project.pbxproj into a property list. Caller releases with
 * CFRelease(). Returns NULL on failure. */
CFTypeRef project_load_pbxproj(const char *project);

/* Resolve a target/config's buildSettings node (borrowed from `root`). */
CFTypeRef project_find_buildsettings(CFTypeRef root, const char *target,
                                     const char *configuration,
                                     char *chosen_name, size_t chosen_len);

/* The project's own build settings, which a target's inherit from. */
CFTypeRef project_find_project_buildsettings(CFTypeRef root,
                                             const char *configuration);

/* The configuration a build uses when none is named on the command line:
 * the project's defaultConfigurationName, or NULL when it names none. */
const char *project_default_configuration(CFTypeRef root, char *buf,
                                          size_t len);

/* The configuration the project's first shared scheme's LaunchAction names,
 * or NULL when the project has no shared scheme.  Outranks both the default
 * above and the record of an earlier index build. */
const char *project_scheme_configuration(const char *project, char *buf,
                                          size_t len);

/* The productType of a target (the first, when target is NULL). */
void project_target_product_type(CFTypeRef root, const char *target,
                                 char *buf, size_t len);

/* A project's display name: its bundle's, minus the extension. */
void project_display_name(const char *path, char *buf, size_t len);

/* Return the root PBXProject object node for a parsed project (the object
 * referenced by rootObject). Returns NULL if absent. Borrowed from `root`. */
CFTypeRef project_get_project_object(CFTypeRef root);

/* Print the "xcodebuild -list" summary for the given project/workspace. */
/* The projects a workspace refers to.  Caller frees each and the array. */
int workspace_projects(const char *workspace, char ***paths);

/* The targets a scheme's build action names, in order.  0 when the
   scheme has no file of its own: Xcode creates one per target on
   demand, so the name is then simply the target's. */
int project_scheme_targets(const char *project, const char *scheme,
    char ***names, char ***containers);

int project_list(const char *project, const char *workspace, const xcodebuild_opts *opts);

/* ------------------------------------------------------------------ */
/* xcindex-test -- the model the third IDETools product exposes.       */
/*                                                                    */
/* These differ from xcodebuild -list on purpose: xcindex-test lists  */
/* scheme files and target-derived names with no dedup between the    */
/* two sets under a case-sensitive ASCII sort, and suppresses a        */
/* target-derived name when any scheme file's BuildableProductRunnable */
/* resolves to it from the same container -- the OPPOSITE rule from    */
/* xcodebuild's, which consults xcschememanagement.plist's             */
/* SuppressBuildableAutocreation instead.                              */
/* ------------------------------------------------------------------ */

/*
 * The xcindex_* entry points below all take the project as a path to an
 * .xcodeproj bundle.  A NULL project stands for a path that is not one --
 * the pbxproj file itself, or a plain directory -- and every one of them
 * then reports an empty project: no targets, no schemes, nothing that
 * resolves.  That is what xcindex-test does with such a path, and it still
 * reports success.
 */

/* The scheme list xcindex-test prints: every scheme-file name (shared
 * and user schemes) followed by every target-derived name that is not
 * suppressed, concatenated with no dedup and sorted case-sensitively.
 * Caller frees each string and the array. Returns the count. */
int xcindex_scheme_list(const char *project, char ***names);

/* Whether `-scheme <name>` resolves: a scheme file (shared, then any
 * user's) wins; otherwise the name is a target-derived scheme and is
 * usable only when that target exists and is not suppressed.  *from_file
 * is set to 1 when a file carried it.  Returns 1 when it resolves. */
int xcindex_scheme_resolve(const char *project, const char *scheme,
    int *from_file);

/* The targets a scheme selects: for a scheme file, every target one of
 * its actions references -- a BuildActionEntry, a TestableReference or a
 * BuildableProductRunnable, in a known action, are all read, so a scheme
 * that both builds and launches a target names it once; for a
 * target-derived scheme, that one target.  The caller sorts and dedups.
 * Caller frees each string and the array. Returns the count. */
int xcindex_scheme_targets(const char *project, const char *scheme,
    char ***names);

/* Every target of the project, in the order its targets array lists them;
 * names and their object GUIDs.  xcindex-test prints targets in name
 * order rather than this one, so callers sort.  Caller frees each string
 * and the arrays. Returns the count. */
int xcindex_target_list(const char *project, char ***names, char ***guids);

/* The source files a target actually compiles, as absolute paths that
 * exist on disk, in build-phase order.  Caller frees each and the array.
 * Returns the count (0 when the target has no on-disk sources). */
int xcindex_target_sources(const char *project, const char *target,
    char ***paths);

/* Print available SDKs and toolchains from the developer directory. */
void project_show_sdks(const char *devpath);
void project_show_toolchains(const char *devpath);

#endif /* __PROJECT_H__ */
