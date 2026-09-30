/* xcodebuild -- open source reimplementation of Apple's xcodebuild utility
 *
 * Shared declarations (options model + build-settings table).
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

#ifndef __XCODEBUILD_H__
#define __XCODEBUILD_H__

#include <CoreFoundation/CoreFoundation.h>

#include <stddef.h>

/* This project's own version, kept in step with CURRENT_PROJECT_VERSION in
 * project.pbxproj, the loader's PROJECT: marker, and the release tag.  It is
 * not an Xcode build number and does not claim to be one. */
#define XCODEBUILD_VERSION "0.1.0"
#define XCODEBUILD_DEFAULT_DEVELOPER_DIR "/Library/Developer/CommandLineTools"
#define SDK_CFG ".xcdev.dat"

/* ------------------------------------------------------------------ */
/* Build-settings table (ordered, last writer wins per key).           */
/* ------------------------------------------------------------------ */

typedef struct {
	char *key;
	char *value;
} setting_entry;

typedef struct {
	setting_entry *entries;
	size_t count;
	size_t capacity;
} settings_table;

settings_table *settings_create(void);
void settings_destroy(settings_table *t);

int settings_has(const settings_table *t, const char *key);
const char *settings_get(const settings_table *t, const char *key);
const char *settings_get_or(const settings_table *t, const char *key, const char *fallback);

/* Set a key, replacing any existing value. Strings are copied. */
void settings_set(settings_table *t, const char *key, const char *value);

/* Append a key only if it does not already exist (lower precedence). */
void settings_defaults_set(settings_table *t, const char *key, const char *value);

/* Delete a key so that a platform that reports none does not either. */
void settings_remove(settings_table *t, const char *key);

/* Expand $(VAR) and ${VAR} references against this table. The result is
 * returned in a freshly allocated buffer (caller frees). */
char *settings_expand(const settings_table *t, const char *value);

int settings_load_xcconfig(settings_table *t, const char *path);

/* Fill in platform/SDK/toolchain-derived defaults. */
int settings_load_defaults(settings_table *t, const char *devpath,
                           const char *sdkname, const char *toolchain,
                           const char *configuration, const char *arch);

/* Print the table in `xcodebuild -showBuildSettings` format. */
int settings_emit(settings_table *t, int as_json, int pretty);

/* Every table's settings as a single JSON array, for the -alltargets case
 * where emitting one array per target would not parse. */
int settings_emit_json_all(settings_table **tables, size_t n, int pretty);

/* Merge string entries of a plist dict (e.g. a pbxproj buildSettings node)
 * into the settings table. Array/dict values are flattened. */
void settings_merge_plist_dict(settings_table *t, CFTypeRef dict);

/* Re-derive the SDK settings (SDK_DIR/SDK_NAME/SDK_VERSION and the
 * numbered forms) from the SDK that SDKROOT names, so that they describe
 * the SDK in use rather than whichever SDK a scan found first.  A no-op
 * when SDKROOT is still an unresolved name or SDK_DIR already agrees with
 * it.  Call once after the project's and xcconfig's settings are merged,
 * and again after command-line overrides so that an SDKROOT= given on the
 * command line is followed too. */
void settings_sync_sdk_root(settings_table *t);

/* ------------------------------------------------------------------ */
/* Parsed command-line options.                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	/* project/workspace/scheme/target */
	char *project;
	char *workspace;
	char *build_root;	/* set for a workspace: one place for products */
	char *scheme;
	char *target;
	/* every -target in the order given: each is answered on its own, where
	 * "target" above keeps only the last for the callers that want one. */
	char **targets;
	int n_targets;
	char *configuration;

	/* toolchain/sdk/arch selection */
	char *sdk;
	char *toolchain;
	char *arch;
	/* every -arch in the order given: they accumulate into one ARCHS
	 * list, where "arch" above keeps only the last for selection. */
	char **archs;
	int n_archs;

	/* destinations / data / paths */
	char *destination;
	char *xcconfig;
	char *derived_data_path;
	char *archive_path;
	char *export_path;
	char *export_options_plist;
	char *project_dir;

	/* action keywords and flags */
	char *action;        /* build/clean/test/analyze/archive/install/installsrc/run/bench */
	int help;
	int version;
	int all_targets;
	int parallel_targets;
	int json;
	int pretty;
	int quiet;
	int verbose;
	int dry_run;
	int show_build_settings;
	int show_build_settings_for_index;
	int show_sdks;
	int export_archive;
	int show_buildable_products;
	int show_runtime_searchable;
	int list_targets;
	int kill_tests;
	int jobs;
	int allow_provisioning_updates;
	int allow_provisioning_device_registration;
	char *result_bundle_path;

	/* residual non-option arguments */
	int argc;
	char **argv;

	/* command-line build-setting overrides (KEY=VALUE), highest precedence */
	char **overrides;
	size_t n_overrides;
} xcodebuild_opts;

xcodebuild_opts *xbuild_opts_create(void);
void xbuild_opts_free(xcodebuild_opts *o);
void xbuild_opt_add_override(xcodebuild_opts *o, const char *kv);

/* Resolve the active developer directory. Honors DEVELOPER_DIR, then the
 * per-user ~/.xcdev.dat cache, then the compiled-in default. Returns a
 * malloc'd string the caller must free, or NULL on failure. */
char *xbuild_get_developer_path(void);

/* Resolve a short SDK name from a -sdk argument / environment / defaults. */
const char *xbuild_resolve_sdk_name(const xcodebuild_opts *opts, const char *devpath);
const char *xbuild_resolve_toolchain_name(const xcodebuild_opts *opts, const char *devpath, const char *sdkname);

/* The project the options select: -project, else a scheme's project, else the
 * one .xcodeproj in the working directory.  A malloc'd path the caller frees,
 * or NULL when there is none. */
char *xbuild_detect_project(const xcodebuild_opts *opts);

/* index.c -- -showBuildSettingsForIndex: every source file's compile settings,
 * as the index service asks for them. */
int xcodebuild_emit_index_settings(const xcodebuild_opts *opts, const char *devpath);

/* build.c -- compile and link a target's sources. */
settings_table *xbuild_settings_for_target(const xcodebuild_opts *opts,
    const char *devpath, const char *target, const char *project);

/* Build a project.  `only` names the targets outright when the caller
   has already worked them out, as a workspace does; NULL means take
   them from the scheme or target on the command line. */
int build_run(const char *project, settings_table *t,
              const xcodebuild_opts *opts, const char *devpath,
              char **only, int nonly);
void build_apply_product_settings(settings_table *t, const char *product_type);

#endif /* __XCODEBUILD_H__ */
