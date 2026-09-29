/* xcodebuild -- open source reimplementation of Apple's xcodebuild utility
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <pwd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "xcodebuild.h"
#include "cfplist.h"
#include "xcpath.h"
#include "devpath.h"
#include "sdkpath.h"
#include "ini.h"
#include "project.h"

#define XCRUN_DEFAULT_CFG "/usr/local/etc/xcrun.ini"

static const char *progname = "xcodebuild";

static const char *actions[] = {
	"build", "clean", "test", "test-without-building", "analyze",
	"archive", "install", "installsrc", "run", "bench", NULL
};

static int is_action(const char *s)
{
	if (s == NULL)
		return 0;
	for (size_t i = 0; actions[i] != NULL; i++)
		if (strcmp(s, actions[i]) == 0)
			return 1;
	return 0;
}

static int endswith(const char *s, const char *suffix)
{
	size_t ls = strlen(s);
	size_t lss = strlen(suffix);
	return ls >= lss && strcmp(s + ls - lss, suffix) == 0;
}

static void free_strv(char **v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(v[i]);
}

/* Recursive rmdir. */
static int rmtree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return -1;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d == NULL)
			return -1;
		struct dirent *e;
		int rc = 0;
		while ((e = readdir(d)) != NULL) {
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			char sub[PATH_MAX];
			snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
			if (rmtree(sub) != 0)
				rc = -1;
		}
		closedir(d);
		return rmdir(path) == 0 ? rc : -1;
	}
	return unlink(path) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* opts create/free/add                                                */
/* ------------------------------------------------------------------ */

xcodebuild_opts *xbuild_opts_create(void)
{
	return (xcodebuild_opts *)calloc(1, sizeof(xcodebuild_opts));
}

void xbuild_opts_free(xcodebuild_opts *o)
{
	if (o == NULL)
		return;
	free(o->project); free(o->workspace); free(o->build_root);
	free(o->scheme);
	free(o->target); free(o->configuration); free(o->sdk);
	free(o->arch); free(o->toolchain); free(o->destination);
	free(o->xcconfig); free(o->derived_data_path);
	free(o->archive_path); free(o->export_path);
	free(o->export_options_plist); free(o->project_dir);
	free(o->action); free(o->result_bundle_path);
	free_strv(o->overrides, o->n_overrides);
	free(o->overrides);
	free(o->argv);
	free(o);
}

void xbuild_opt_add_override(xcodebuild_opts *o, const char *kv)
{
	if (o == NULL || kv == NULL)
		return;
	char **grow = (char **)realloc(o->overrides, sizeof(char *) * (o->n_overrides + 1));
	if (grow == NULL)
		return;
	o->overrides = grow;
	o->overrides[o->n_overrides++] = strdup(kv);
}

/* ------------------------------------------------------------------ */
/* ini value readback                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *section;
	const char *key;
	char *out;
	size_t outsz;
	int found;
} ini_val_ctx;

static int ini_val_handler(void *user, const char *section, const char *name, const char *value)
{
	ini_val_ctx *c = (ini_val_ctx *)user;
	if (!c->found && strcmp(section, c->section) == 0 && strcmp(name, c->key) == 0) {
		snprintf(c->out, c->outsz, "%s", value);
		c->found = 1;
	}
	return 1;
}

/* Read a scalar `key` from `section` of an ini file. Returns 1 if found,
 * 0 if not, -1 if the file could not be opened. */
static int ini_get_value(const char *path, const char *section, const char *key,
                         char *out, size_t outsz)
{
	ini_val_ctx c = { section, key, out, outsz, 0 };
	int rc = ini_parse(path, ini_val_handler, &c);
	return rc == -1 ? -1 : c.found;
}

/* ------------------------------------------------------------------ */
/* Usage / version                                                     */
/* ------------------------------------------------------------------ */

static void usage(FILE *fp, int code)
{
	fprintf(fp, "Usage: %s [options] [action]\n", progname);
	fputs("A reimplementation of Apple's xcodebuild utility.\n\n"
	      "Action (defaults to 'build'): build, clean, test, test-without-building,\n"
	      "  analyze, archive, install, installsrc, run, bench.\n\n"
	      "Options:\n"
	      "  -project <project>            specify the project to operate on\n"
	      "  -workspace <workspace>        specify the workspace\n"
	      "  -scheme <scheme>             build the specified scheme\n"
	      "  -target <target>             build the specified target\n"
	      "  -configuration <cfg>         use the named build configuration\n"
	      "  -sdk <sdk>                   use the specified SDK (name or path)\n"
	      "  -arch <arch>                 build for the specified architecture\n"
	      "  -toolchain <name>            use the specified toolchain\n"
	      "  -destination <dest>          select the destination device/runner\n"
	      "  -xcconfig <file>             apply build settings from this file\n"
	      "  -derivedDataPath <path>      where to put derived data\n"
	      "  -archivePath <path>          specify the archive path\n"
	      "  -exportPath <path>           where to write the exported product\n"
	      "  -exportOptionsPlist <file>   plist describing the export\n"
	      "  -projectDir <dir>            path to the project directory\n"
	      "  -resultBundlePath <path>\n"
	      "  -alltargets                  build all targets\n"
	      "  -parallelizeTargets          parallelize target builds\n"
	      "  -jobs <n>                    number of build jobs\n"
	      "  -allowProvisioningUpdates\n"
	      "  -allowProvisioningDeviceRegistration\n"
	      "  -quiet                       do not print console output\n"
	      "  -verbose / -v                provide additional status output\n"
	      "  -dry-run                     print commands without executing\n"
	      "  -kill-tests                  delete runs from the test plan\n"
	      "  -json                        output JSON (with -showBuildSettings)\n"
	      "  -pretty                      pretty-print JSON\n"
	      "  -list                        list project information\n"
	      "  -showBuildSettings           print build settings\n"
	      "  -showsdks                    list available SDKs\n"
	      "  -showBuildableProducts       list buildable products\n"
	      "  -exportArchive               export an archive\n"
	      "  -h, -help, --help            show this help\n"
	      "  --version / -version         show version information\n\n"
	      "Build settings may also be specified on the command line as KEY=VALUE.\n",
	      fp);
	exit(code);
}

static void version_print(int verbose)
{
	fprintf(stdout, "xcodebuild %s\n", XCODEBUILD_VERSION);
	if (verbose) {
		char *devpath = xbuild_get_developer_path();
		if (devpath != NULL) {
			fprintf(stdout, "Build version 0.1.0\n");
			fprintf(stdout, "Developer path: %s\n", devpath);
			free(devpath);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Developer path resolution                                           */
/* ------------------------------------------------------------------ */

char *xbuild_get_developer_path(void)
{
	const char *value = getenv("DEVELOPER_DIR");
	if (value != NULL && *value)
		return strdup(value);

	char *home = getenv("HOME");
	if (home == NULL) {
		fprintf(stderr, "xcodebuild: error: unable to determine HOME directory\n");
		return NULL;
	}

	char cfg_path[PATH_MAX];
	snprintf(cfg_path, sizeof(cfg_path), "%s/%s", home, SDK_CFG);
	FILE *fp = fopen(cfg_path, "r");
	if (fp != NULL) {
		char devpath[PATH_MAX];
		memset(devpath, 0, sizeof(devpath));
		size_t n = fread(devpath, 1, PATH_MAX - 1, fp);
		fclose(fp);
		devpath[n] = '\0';
		while (n > 0 && (devpath[n - 1] == '\n' || devpath[n - 1] == '\r' ||
		                 devpath[n - 1] == ' ' || devpath[n - 1] == '\t'))
			devpath[--n] = '\0';
		if (devpath[0] != '\0')
			return strdup(devpath);
	}

	struct stat st;
	/*
	 * Prefer the Developer directory this binary lives in, so a
	 * relocated or freshly built release tree works with no
	 * configuration; the compiled-in system default is the fallback.
	 */
	{
		const char *self = xt_default_developer_dir();

		if (self != NULL)
			return strdup(self);
	}

	if (stat(XCODEBUILD_DEFAULT_DEVELOPER_DIR, &st) == 0 && S_ISDIR(st.st_mode))
		return strdup(XCODEBUILD_DEFAULT_DEVELOPER_DIR);

	fprintf(stderr, "xcodebuild: error: unable to locate a developer directory.\n"
	                "xcodebuild: error: run `xcode-select --switch <path>` or set DEVELOPER_DIR.\n");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* SDK / toolchain name resolution                                     */
/* ------------------------------------------------------------------ */

const char *xbuild_resolve_sdk_name(const xcodebuild_opts *opts, const char *devpath)
{
	static char sdk[PATH_MAX];

	if (opts != NULL && opts->sdk != NULL) {
		/*
		 * An absolute -sdk is kept whole.  Reducing it to a base
		 * name sent the lookup back into the developer directory,
		 * so pointing a build at an SDK anywhere else quietly got
		 * the local one of the same name instead.
		 */
		if (opts->sdk[0] == '/') {
			snprintf(sdk, sizeof(sdk), "%s", opts->sdk);
			return sdk;
		}
		snprintf(sdk, sizeof(sdk), "%s", opts->sdk);
		return sdk;
	}

	const char *env = getenv("SDKROOT");
	if (env != NULL && *env) {
		if (env[0] == '/') {
			snprintf(sdk, sizeof(sdk), "%s", env);
			return sdk;
		}
		snprintf(sdk, sizeof(sdk), "%s", env);
		return sdk;
	}

	if (devpath != NULL) {
		char ini_path[PATH_MAX];
		{
			const char *self = xt_default_developer_dir();
			struct stat cst;

			/* Prefer the copy inside the developer directory. */
			if (self != NULL) {
				snprintf(ini_path, sizeof(ini_path),
					 "%s/usr/share/xcrun.ini", self);
				if (stat(ini_path, &cst) == 0 && S_ISREG(cst.st_mode))
					goto have_ini;
			}
		}
		snprintf(ini_path, sizeof(ini_path), "%s", XCRUN_DEFAULT_CFG);
have_ini:;
		char name[256] = {0};
		if (ini_get_value(ini_path, "SDK", "name", name, sizeof(name)) == 1 && name[0]) {
			snprintf(sdk, sizeof(sdk), "%s", name);
			return sdk;
		}
		/* Platform-aware, and prefers the host's SDK. */
		char *found = xt_first_sdk_name(devpath);

		if (found != NULL) {
			snprintf(sdk, sizeof(sdk), "%s", found);
			free(found);
			return sdk;
		}
	}
	snprintf(sdk, sizeof(sdk), "MacOSX");
	return sdk;
}

const char *xbuild_resolve_toolchain_name(const xcodebuild_opts *opts, const char *devpath, const char *sdkname)
{
	static char tc[PATH_MAX];

	if (opts != NULL && opts->toolchain != NULL) {
		snprintf(tc, sizeof(tc), "%s", opts->toolchain);
		return tc;
	}

	const char *env = getenv("TOOLCHAINS");
	if (env != NULL && *env) {
		const char *base = strrchr(env, '/');
		const char *start = base ? base + 1 : env;
		const char *dot = strstr(start, ".toolchain");
		size_t len = dot ? (size_t)(dot - start) : strlen(start);
		snprintf(tc, sizeof(tc), "%.*s", (int)len, start);
		return tc;
	}

	if (devpath != NULL && sdkname != NULL) {
		char *sdk_path = xt_find_sdk(devpath, sdkname);
		char name[256] = {0};

		/*
		 * An SDK in the older layout names its toolchain in info.ini.
		 * SDKSettings.plist has no equivalent: in Apple's layout the
		 * toolchain belongs to the Developer directory, not the SDK.
		 */
		if (sdk_path != NULL) {
			int got = ini_get_value(sdk_path, "SDK", "toolchain",
						name, sizeof(name));

			free(sdk_path);
			if (got == 1 && name[0]) {
				snprintf(tc, sizeof(tc), "%s", name);
				return tc;
			}
		}
	}

	/* XcodeDefault is the name a stock toolchain carries, and the one
	 * our own bundles emit. */
	if (devpath != NULL) {
		char *path = xt_find_toolchain(devpath, "XcodeDefault");

		if (path != NULL) {
			free(path);
			snprintf(tc, sizeof(tc), "XcodeDefault");
			return tc;
		}
	}

	snprintf(tc, sizeof(tc), "%s", sdkname ? sdkname : "XcodeDefault");
	return tc;
}

static char *path_join(const char *a, const char *b)
{
	char *p = (char *)malloc(PATH_MAX);
	if (p)
		snprintf(p, PATH_MAX, "%s/%s", a, b);
	return p;
}

static char *detect_project(const xcodebuild_opts *opts, const char *project_dir)
{
	if (opts != NULL && opts->project != NULL)
		return strdup(opts->project);
	const char *dir = project_dir ? project_dir : ".";
	DIR *d = opendir(dir);
	if (d == NULL)
		return NULL;
	struct dirent *e;
	char *found = NULL;
	while ((e = readdir(d)) != NULL) {
		if (endswith(e->d_name, ".xcodeproj")) {
			found = path_join(dir, e->d_name);
			break;
		}
	}
	closedir(d);
	return found;
}

/* ------------------------------------------------------------------ */
/* Settings orchestration                                            */
/* ------------------------------------------------------------------ */

/*
 * Apply the command line's SETTING=value overrides.
 *
 * Called twice: once so that a SETTING on the command line wins over
 * the project's, the xcconfig's and this tool's own defaults, and once
 * more after the SDK identity is derived, so that it also wins over
 * that derivation.  Applying a key twice is the same as applying it
 * once.
 */
static void apply_setting_overrides(settings_table *t,
    const xcodebuild_opts *opts)
{
	size_t i;

	for (i = 0; i < opts->n_overrides; i++) {
		const char *eq = strchr(opts->overrides[i], '=');
		if (eq != NULL) {
			char key[256];
			char *expanded;

			snprintf(key, sizeof(key), "%.*s",
			         (int)(eq - opts->overrides[i]),
			         opts->overrides[i]);
			expanded = settings_expand(t, eq + 1);
			settings_set(t, key, expanded);
			free(expanded);
		}
	}
}

/* Prepend a search-path element to a possibly empty list.  Apple keeps the
 * separator in the prefix, so an empty list comes out with a trailing space
 * rather than nothing -- matching that keeps the value byte-identical. */
static void prefix_search_path(char *out, size_t outsz, const char *prefix,
    const char *list)
{
	if (list == NULL)
		list = "";
	snprintf(out, outsz, "%s%s", prefix, list);
}

/*
 * The build directories, derived from the settings that survive the merge.
 *
 * These are seeded with sensible values before the project's settings are
 * merged, because a project writes its own in terms of them and they have to
 * expand to something.  But a seed is only a guess: a project is free to put
 * its intermediates somewhere else, and ours does --
 * OBJROOT = $(SRCROOT)/build/obj/$(CONFIGURATION) and
 * CONFIGURATION_BUILD_DIR = $(SRCROOT)/build/release -- so the directories
 * derived from the seed point at the wrong place and Apple disagrees on
 * thirteen of them.  Reading the merged values back and deriving again is what
 * makes OBJROOT authoritative, which is where Apple puts the whole chain.
 */
/*
 * The -<platform> suffix Apple puts on a configuration's directory name, and
 * the reason this document used to say it could not be reproduced.
 *
 * It used to say: reproducing it needs a platform name that tracks -sdk, and
 * PLATFORM_NAME does not, so the default-platform form is emitted.  That is
 * true of PLATFORM_NAME and it was the wrong thing to conclude, because the
 * name is not only available in PLATFORM_NAME.  It is in the directory the SDK
 * was found in: .../Platforms/<Name>.platform/Developer/SDKs/<sdk>, and
 * lowercasing <Name> is the suffix exactly.  Verified against Apple for macosx
 * (-> no suffix at all), iphoneos, iphonesimulator, appletvos,
 * appletvsimulator, watchos, watchsimulator, xros and xrsimulator, and for all
 * of those reached by SDK path as well as by name, so the version and the
 * .sdk suffix never enter into it.  A path to the iOS SDK gets -iphoneos; a
 * path to the macOS SDK, and the macOS SDK by any spelling, get nothing.
 *
 * Reading it out of SDKROOT is deliberate: SDKROOT already resolves correctly
 * on every platform measured, so this asks the same question of a value that is
 * known to be right rather than of one that is known to be wrong.
 */
static void platform_suffix(const settings_table *t, char *out, size_t outsz)
{
	const char *sdkroot;
	const char *p, *start;

	out[0] = '\0';

	sdkroot = settings_get(t, "SDKROOT");
	if (sdkroot == NULL || *sdkroot == '\0')
		return;

	p = strstr(sdkroot, "/Platforms/");
	if (p == NULL)
		return;
	start = p + strlen("/Platforms/");
	p = strstr(start, ".platform");
	if (p == NULL || p == start)
		return;

	if ((size_t)(p - start) + 1 >= outsz)
		return;

	/*
	 * macOS is the default platform and carries no suffix; every other
	 * one does.  Compared case-insensitively because the directory is
	 * MacOSX and the answer to write is macosx.
	 */
	if ((size_t)(p - start) == 6 && strncasecmp(start, "MacOSX", 6) == 0)
		return;

	snprintf(out, outsz, "-%.*s", (int)(p - start), start);
	{
		char *w;

		for (w = out + 1; *w != '\0'; w++)
			*w = (char)tolower((unsigned char)*w);
	}
}

static void derive_build_dirs(settings_table *t)
{
	const char *objroot, *symroot, *cfg_build, *config;
	char proj_dir[PATH_MAX], cfg_dir[PATH_MAX];
	char suffix[32];
	const char *pname, *tname;

	objroot = settings_get(t, "OBJROOT");
	if (objroot == NULL || *objroot == '\0')
		objroot = settings_get(t, "SYMROOT");
	if (objroot == NULL || *objroot == '\0')
		return;

	pname = settings_get(t, "PROJECT_NAME");
	if (pname == NULL || *pname == '\0')
		pname = "project";

	snprintf(proj_dir, sizeof(proj_dir), "%s/%s.build", objroot, pname);
	settings_set(t, "PROJECT_TEMP_DIR", proj_dir);
	settings_set(t, "PROJECT_TEMP_ROOT", objroot);
	settings_set(t, "TEMP_ROOT", objroot);

	config = settings_get(t, "CONFIGURATION");
	if (config == NULL || *config == '\0')
		config = "Release";

	/*
	 * The configuration's own directory is named Release, and
	 * Release-iphoneos, and Release-watchos, and so on for every
	 * platform that is not the default one.  Everything derived from it
	 * below -- the target directory, the object files, the linker map --
	 * moves with it, which is why this is computed once here.
	 */
	platform_suffix(t, suffix, sizeof suffix);

	snprintf(cfg_dir, sizeof(cfg_dir), "%s/%s%s", proj_dir, config,
	    suffix);
	settings_set(t, "CONFIGURATION_TEMP_DIR", cfg_dir);

	/*
	 * Five more directories are OBJROOT plus a fixed tail -- the same shape
	 * as PROJECT_TEMP_DIR above, and spelled out rather than derived from
	 * the key names, because the names do not imply the location:
	 * SHARED_PRECOMPS_DIR sits nowhere near SHARED_anything, and
	 * COMPOSITE_SDK_DIRS is plural where GENERATED_MODULEMAP_DIR is
	 * singular.  They are derived here rather than seeded so that an
	 * OBJROOT= override moves them too, which is the whole point of
	 * making OBJROOT authoritative.
	 *
	 * One of the five does not have a fixed tail: Apple emits
	 * GeneratedModuleMaps on the default platform and
	 * GeneratedModuleMaps-iphoneos on another, so it takes the same
	 * -<platform> suffix as the configuration directory.  This used to
	 * be documented as not reproducible, on the grounds that it needs a
	 * platform name that tracks -sdk and PLATFORM_NAME does not; see
	 * platform_suffix() above, which finds the name somewhere that does
	 * track it.
	 */
	{
		static const struct {
			const char *key, *tail;
			int suffixed;		/* takes the -<platform> suffix */
		} d[] = {
			{ "COMPOSITE_SDK_DIRS",		"CompositeSDKs",		0 },
			{ "GENERATED_MODULEMAP_DIR",	"GeneratedModuleMaps",	1 },
			{ "SHARED_PRECOMPS_DIR",	"SharedPrecompiledHeaders", 0 },
			{ "TEMP_SANDBOX_DIR",		"TemporaryTaskSandboxes",	0 },
			{ "UNINSTALLED_PRODUCTS_DIR",	"UninstalledProducts",	0 },
		};
		char path[PATH_MAX];
		size_t i;

		for (i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
			if (d[i].suffixed)
				snprintf(path, sizeof(path), "%s/%s%s",
				    objroot, d[i].tail, suffix);
			else
				snprintf(path, sizeof(path), "%s/%s",
				    objroot, d[i].tail);
			settings_set(t, d[i].key, path);
		}
	}

	/*
	 * Where the products go.  TARGET_BUILD_DIR follows
	 * CONFIGURATION_BUILD_DIR, which a project may have pointed
	 * somewhere else; Apple resolves it the same way, so a project
	 * that sets one gets both.
	 */
	symroot = settings_get(t, "SYMROOT");
	if (symroot != NULL && *symroot != '\0') {
		settings_set(t, "BUILD_DIR", symroot);
		settings_set(t, "BUILD_ROOT", symroot);
	}

	cfg_build = settings_get(t, "CONFIGURATION_BUILD_DIR");
	if (cfg_build != NULL && *cfg_build != '\0') {
		settings_set(t, "BUILT_PRODUCTS_DIR", cfg_build);
		settings_set(t, "TARGET_BUILD_DIR", cfg_build);

		/*
		 * These two are seeded from BUILT_PRODUCTS_DIR by
		 * build_apply_product_settings(), which runs before
		 * the merge, so they latched the seed.  Their base has
		 * just moved, so they move with it.
		 */
		{
			const char *full = settings_get(t, "FULL_PRODUCT_NAME");
			char path[PATH_MAX];

			if (full != NULL && *full != '\0') {
				snprintf(path, sizeof(path), "%s/%s",
				    cfg_build, full);
				settings_set(t, "CODESIGNING_FOLDER_PATH", path);
			}
			settings_set(t, "DWARF_DSYM_FOLDER_PATH", cfg_build);
		}

		/*
		 * Two more off the products directory.  METAL_LIBRARY_OUTPUT_DIR
		 * is the directory with a trailing slash and nothing after it,
		 * which is exactly what Apple emits; trimming it reads as a
		 * cleanup and is a mismatch.
		 *
		 * It is not platform-gated, which was the second wrong guess
		 * here: Apple emits it for macOS, iOS, tvOS and watchOS alike.
		 * So it is emitted unconditionally, and deliberately not gated on
		 * PLATFORM_NAME -- that key does not track -sdk, reporting macosx
		 * for every platform, so a gate on it would be a gate on a lie.
		 */
		{
			char path[PATH_MAX];

			snprintf(path, sizeof(path), "%s/DerivedSources", cfg_build);
			settings_set(t, "SHARED_DERIVED_FILE_DIR", path);

			snprintf(path, sizeof(path), "%s/", cfg_build);
			settings_set(t, "METAL_LIBRARY_OUTPUT_DIR", path);
		}

		/*
		 * The built products directory goes on the front of both
		 * search paths, which is how one target finds a framework
		 * another has just built.  ENABLE_DEFAULT_HEADER_SEARCH_PATHS
		 * gates it: with it off Apple leaves both lists exactly as
		 * the project wrote them, and with it on it prepends to
		 * whatever the project wrote rather than replacing it, so
		 * FRAMEWORK_SEARCH_PATHS=/custom/fw comes out as
		 * "<products> /custom/fw".
		 *
		 * The prepend is unconditional, so a project that already
		 * lists the include directory gets it twice -- that is what
		 * Apple does, and matching it beats second-guessing it.
		 * It has to happen here rather than at the seed because the
		 * project has to have been read first: the value being
		 * prepended to is the project's own.
		 */
		{
			const char *dhsp = settings_get(t,
			    "ENABLE_DEFAULT_HEADER_SEARCH_PATHS");
			const char *hsp = settings_get(t, "HEADER_SEARCH_PATHS");
			const char *fsp = settings_get(t, "FRAMEWORK_SEARCH_PATHS");
			char path[PATH_MAX], out[PATH_MAX];

			if (dhsp != NULL && strcasecmp(dhsp, "YES") == 0) {
				snprintf(path, sizeof(path), "%s/include ",
				    cfg_build);
				prefix_search_path(out, sizeof(out), path, hsp);
				settings_set(t, "HEADER_SEARCH_PATHS", out);

				snprintf(path, sizeof(path), "%s ", cfg_build);
				prefix_search_path(out, sizeof(out), path, fsp);
				settings_set(t, "FRAMEWORK_SEARCH_PATHS", out);
			}
		}
	}

	/* A target's own directory, hanging off the configuration's. */
	tname = settings_get(t, "TARGET_NAME");
	if (tname != NULL && *tname != '\0') {
		char tgt[PATH_MAX], sub[PATH_MAX];

		snprintf(tgt, sizeof(tgt), "%s/%s.build", cfg_dir, tname);
		settings_set(t, "TARGET_TEMP_DIR", tgt);
		settings_set(t, "TEMP_DIR", tgt);
		settings_set(t, "TEMP_FILE_DIR", tgt);
		settings_set(t, "TEMP_FILES_DIR", tgt);
		settings_set(t, "STRINGSDATA_ROOT", tgt);

		snprintf(sub, sizeof(sub), "%s/Objects", tgt);
		settings_set(t, "OBJECT_FILE_DIR", sub);

		snprintf(sub, sizeof(sub), "%s/Objects-normal", tgt);
		settings_set(t, "OBJECT_FILE_DIR_normal", sub);

		/*
		 * The strings step is per-architecture, and macOS has no
		 * per-architecture strings step, so Apple leaves the slot
		 * literally undefined rather than filling it with an arch
		 * it does not use.  Verified constant across Debug and
		 * Release and across ARCHS=arm64 / x86_64 / both.  Only
		 * macOS is verified; a platform that really does split
		 * per architecture would want the arch here instead.
		 */
		snprintf(sub, sizeof(sub), "%s/Objects-normal/undefined_arch", tgt);
		settings_set(t, "STRINGSDATA_DIR", sub);

		snprintf(sub, sizeof(sub), "%s/DerivedSources", tgt);
		settings_set(t, "DERIVED_FILE_DIR", sub);
		settings_set(t, "DERIVED_FILES_DIR", sub);
		settings_set(t, "DERIVED_SOURCES_DIR", sub);

		snprintf(sub, sizeof(sub), "%s/JavaClasses", tgt);
		settings_set(t, "CLASS_FILE_DIR", sub);

		snprintf(sub, sizeof(sub), "%s/FixedFiles", tgt);
		settings_set(t, "FIXED_FILES_DIR", sub);

		/*
		 * Sixteen more keys, and the largest remaining group is this
		 * one: everything else under the target's own directory.  The
		 * paths are not one rule but two, and the split is the
		 * interesting part.
		 *
		 * One group is spelled with CURRENT_ARCH, which in
		 * -showBuildSettings is "undefined_arch" -- and stays
		 * "undefined_arch" whatever ARCHS is set to, verified across
		 * arm64, x86_64, arm64+x86_64, arm64+arm64e and an empty
		 * list.  It is a property of asking for settings without
		 * asking for a build, not of the target.
		 *
		 * The other group is spelled with the architecture itself,
		 * one key per entry in ARCHS, subscripted into the key name
		 * and used as a directory component in the value.  So with
		 * ARCHS=arm64 x86_64 there are six of them, and with an
		 * empty ARCHS there are none.  Note the two groups disagree
		 * about what an architecture is -- the first says
		 * undefined_arch where the second says arm64 -- because Apple
		 * resolves them at different times.
		 */
		{
			char objs[PATH_MAX], archdir[PATH_MAX];
			const char *arch;
			size_t i;

			snprintf(objs, sizeof(objs), "%s/Objects-normal", tgt);
			settings_set(t, "PER_VARIANT_OBJECT_FILE_DIR", objs);

			arch = settings_get(t, "CURRENT_ARCH");
			if (arch != NULL && *arch != '\0') {
				snprintf(archdir, sizeof(archdir),
				         "%s/%s", objs, arch);
				settings_set(t, "PER_ARCH_OBJECT_FILE_DIR",
				    archdir);
				settings_set(t, "PER_ARCH_MODULE_FILE_DIR",
				    archdir);

				snprintf(sub, sizeof(sub),
				    "%s/Processed-Info.plist", archdir);
				settings_set(t, "PROCESSED_INFOPLIST_PATH", sub);

				snprintf(sub, sizeof(sub),
				    "%s/%s_dependency_info.dat", archdir, tname);
				settings_set(t, "LD_DEPENDENCY_INFO_FILE", sub);

				snprintf(sub, sizeof(sub),
				    "%s/%s-LinkMap-normal-%s.txt", tgt, tname,
				    arch);
				settings_set(t, "LD_MAP_FILE_PATH", sub);
			}

			/* Five more with neither the name nor an arch in them. */
			{
				static const struct {
					const char *key, *tail;
				} d[] = {
					{ "FILE_LIST",			"Objects/LinkFileList" },
					{ "PKGINFO_FILE_PATH",		"PkgInfo" },
					{ "PRECOMP_DESTINATION_DIR",	"PrefixHeaders" },
					{ "REZ_COLLECTOR_DIR",		"ResourceManagerResources" },
					{ "REZ_OBJECTS_DIR",		"ResourceManagerResources/Objects" },
				};

				for (i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
					snprintf(sub, sizeof(sub), "%s/%s",
					    tgt, d[i].tail);
					settings_set(t, d[i].key, sub);
				}
			}

			snprintf(sub, sizeof(sub),
			    "%s/%s-BuildDependencyInfo.json", tgt, tname);
			settings_set(t, "DUMP_DEPENDENCIES_OUTPUT_PATH", sub);

			/*
			 * The three per-architecture files.  The key is
			 * the family name with _normal_<arch> on the end,
			 * so the table carries the stem rather than a
			 * key, and the arch is not something that can be
			 * interpolated into a static string.
			 */
			{
				static const struct {
					const char *stem, *ext;
				} pa[] = {
					{ "LINK_FILE_LIST",			".LinkFileList" },
					{ "LM_AUX_CONST_METADATA_LIST_PATH",	".SwiftConstValuesFileList" },
					{ "SWIFT_RESPONSE_FILE_PATH",		".SwiftFileList" },
				};
				const char *archs = settings_get(t, "ARCHS");

				if (archs != NULL && *archs != '\0') {
					char list[PATH_MAX], *save, *tok;

					snprintf(list, sizeof(list), "%s", archs);
					for (tok = strtok_r(list, " \t", &save);
					     tok != NULL;
					     tok = strtok_r(NULL, " \t", &save)) {
						char dir[PATH_MAX];

						snprintf(dir, sizeof(dir),
						    "%s/%s", objs, tok);
						for (i = 0; i < sizeof(pa) /
						    sizeof(pa[0]); i++) {
							char key[128];

							snprintf(key, sizeof(key),
							    "%s_normal_%s",
							    pa[i].stem, tok);
							snprintf(sub, sizeof(sub),
							    "%s/%s%s", dir,
							    tname, pa[i].ext);
							settings_set(t, key, sub);
						}
					}
				}
			}
		}

		/*
		 * One that looks like the block above and is not in it.
		 * PROJECT_DERIVED_FILE_DIR hangs off the *project's*
		 * directory, so it has neither the configuration nor the
		 * target in its path, where DERIVED_FILE_DIR has both.
		 * Apple keeps the two one word apart; matching either to
		 * the other is the mistake this comment is for.
		 */
		snprintf(sub, sizeof(sub), "%s/DerivedSources", proj_dir);
		settings_set(t, "PROJECT_DERIVED_FILE_DIR", sub);
	}
}

/*
 * MD5, as lowercase hex.
 *
 * Apple builds a filename component out of the digest of the SDK's path, and
 * the digest it uses is MD5.  CommonCrypto still has it, marked deprecated
 * since 10.15 for the cryptographic reason, which does not apply here: this
 * reproduces a name Apple already chose, it protects nothing, and a different
 * digest would name a file that does not exist.  Rather than suppress the
 * warning or take a new dependency it is implemented here, where the reason for
 * using a broken hash sits next to it.  The test suite checks it against the
 * system's own md5.
 */
static uint32_t md5_rot(uint32_t x, unsigned r)
{
	return (x << r) | (x >> (32 - r));
}

static void md5_block(uint32_t h[4], const uint8_t block[64])
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf,
		0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
		0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e,
		0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
		0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
		0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
		0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
		0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
		0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
		0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
		0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
	};
	static const uint8_t R[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
	};
	uint32_t m[16];
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
	int r;

	for (r = 0; r < 16; r++)
		m[r] = (uint32_t)block[r * 4] |
		    ((uint32_t)block[r * 4 + 1] << 8) |
		    ((uint32_t)block[r * 4 + 2] << 16) |
		    ((uint32_t)block[r * 4 + 3] << 24);

	for (r = 0; r < 64; r++) {
		uint32_t f, g, sum, tmp;

		if (r < 16) {
			f = (b & c) | (~b & d);
			g = (unsigned)r;
		} else if (r < 32) {
			f = (d & b) | (~d & c);
			g = (5 * (unsigned)r + 1) & 15;
		} else if (r < 48) {
			f = b ^ c ^ d;
			g = (3 * (unsigned)r + 5) & 15;
		} else {
			f = c ^ (b | ~d);
			g = (7 * (unsigned)r) & 15;
		}

		tmp = d;
		d = c;
		c = b;
		sum = a + f + K[r] + m[g];
		b += md5_rot(sum, R[r]);
		a = tmp;
	}

	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
}

static void md5_hex(const void *data, size_t len, char *out /* 33 bytes */)
{
	const uint8_t *p = data;
	uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	uint64_t bits = (uint64_t)len * 8;
	uint8_t block[64];
	size_t used = 0, n, i;

	while (used + 64 <= len) {
		md5_block(h, p + used);
		used += 64;
	}

	/* 0x80, zeroes, then the length in bits, little-endian. */
	n = len - used;
	memcpy(block, p + used, n);
	block[n++] = 0x80;
	if (n > 56) {
		while (n < 64)
			block[n++] = 0;
		md5_block(h, block);
		n = 0;
	}
	while (n < 56)
		block[n++] = 0;
	for (i = 0; i < 8; i++)
		block[56 + i] = (uint8_t)(bits >> (8 * i));
	md5_block(h, block);

	/*
	 * The digest is the four state words serialized little-endian, so
	 * "%08x%08x%08x%08x" over the words is the byte-reverse of the real
	 * digest -- a plausible-looking wrong hash, not an obvious failure.
	 * Take the bytes lowest-first out of each word, then hex them: the
	 * caller wants a lowercase hex string for a filename, not the raw
	 * 16 bytes.
	 */
	for (i = 0; i < 4; i++) {
		uint32_t v = h[i];
		size_t b;

		for (b = 0; b < 4; b++) {
			unsigned byte = (v >> (8 * b)) & 0xff;

			*out++ = "0123456789abcdef"[byte >> 4];
			*out++ = "0123456789abcdef"[byte & 0xf];
		}
	}
	*out = '\0';
}

/*
 * Nine keys that all answer one question: what is this thing called.
 *
 * They are one group because they are all read off two things the project
 * already states -- its own filename and its target's productType -- and
 * because four of the nine are just those two things renamed.  Verified
 * against Apple for both of this project's targets, a tool and a dynamic
 * library, which is two data points and is called out below.
 *
 * The one that looks like a hash of nothing is PROJECT_GUID, and it is a hash
 * of very little: the MD5 of the project's *filename*, extension included.  Not
 * its contents, and not its directory -- measured both ways, by copying the
 * project under a different name (a different GUID) and by reaching the same
 * file through a symlink (the same one).  The filename is canonicalized first,
 * so a symlink named Alias.xcodeproj pointing at IDETools.xcodeproj still
 * reports the GUID of IDETools.xcodeproj; taking the basename of the path as
 * typed gets that wrong, and it is the kind of wrong that only shows up when
 * somebody reaches the project through a symlink.
 *
 * Two of the nine are consumed-but-not-reported dependencies of others.
 * VERSION_INFO_FILE is named after $(PRODUCT_NAME), not $(TARGET_NAME), which
 * for a target that renames its product is a different string; and
 * VERSION_INFO_STRING ends in $(CURRENT_PROJECT_VERSION), which Apple
 * interpolates into it and does not itself report for a target that sets none
 * -- so the version-less form has a trailing hyphen and nothing after it,
 * which looks like a typo and is not.
 *
 * The two productType-dependent keys are a table of two, and that is the
 * whole table as far as it has been measured.  A productType not in it emits
 * neither key rather than a guess: PACKAGE_TYPE and STRIP_STYLE are both
 * wrong-looking values, and a project with a different product type is
 * exactly the case where being confidently wrong is most expensive.
 */
static void derive_identity(settings_table *t, CFTypeRef root,
    const char *project, const char *target)
{
	static const struct {
		const char *product_type;
		const char *package_type;
		const char *strip_style;
	} kinds[] = {
		{ "com.apple.product-type.tool",
		  "com.apple.package-type.mach-o-executable",	"all" },
		{ "com.apple.product-type.library.dynamic",
		  "com.apple.package-type.mach-o-dylib",		"debugging" },
	};
	char resolved[PATH_MAX], name[PATH_MAX], guid[33], ptype[256];
	const char *pname, *tname, *product, *cver, *slash;
	struct passwd *pw;
	size_t i;

	if (root == NULL || project == NULL)
		return;

	/*
	 * The two names.  Both are read out of the table rather than
	 * recomputed, so a project that renames itself or its target gets
	 * the renamed pair here too.
	 */
	pname = settings_get(t, "PROJECT_NAME");
	tname = settings_get(t, "TARGET_NAME");
	if (pname != NULL && *pname != '\0')
		settings_defaults_set(t, "PROJECT", pname);
	if (tname != NULL && *tname != '\0')
		settings_defaults_set(t, "TARGETNAME", tname);

	/*
	 * The GUID, from the canonical filename.  realpath() rather than the
	 * path as typed, because Apple resolves the symlink first.
	 */
	if (realpath(project, resolved) != NULL &&
	    (slash = strrchr(resolved, '/')) != NULL && slash[1] != '\0') {
		snprintf(name, sizeof(name), "%s", slash + 1);
		md5_hex(name, strlen(name), guid);
		settings_defaults_set(t, "PROJECT_GUID", guid);
	}

	/*
	 * What it builds, and how that product is stripped.  The target
	 * comes out of the table rather than off the command line, so that
	 * these two describe the same target as the rest of the settings:
	 * with no -target the table holds the selected default, while a
	 * NULL target would send the lookup to the first target in the
	 * project's own order, which is not necessarily the same one.
	 */
	project_target_product_type(root,
	    (tname != NULL && *tname != '\0') ? tname : target, ptype,
	    sizeof(ptype));
	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		if (strcmp(ptype, kinds[i].product_type) != 0)
			continue;
		settings_defaults_set(t, "PACKAGE_TYPE", kinds[i].package_type);
		settings_defaults_set(t, "STRIP_STYLE", kinds[i].strip_style);
		break;
	}

	/* The version-info file, named after the product. */
	product = settings_get(t, "PRODUCT_NAME");
	if (product != NULL && *product != '\0') {
		char vfile[PATH_MAX];

		snprintf(vfile, sizeof(vfile), "%s_vers.c", product);
		settings_defaults_set(t, "VERSION_INFO_FILE", vfile);
	}

	/*
	 * Who built it: the login name from the password database, which is
	 * not $USER.  With USER set to something else, and with it unset
	 * entirely, Apple still reports the account name.
	 */
	pw = getpwuid(getuid());
	if (pw != NULL && pw->pw_name != NULL && *pw->pw_name != '\0')
		settings_defaults_set(t, "VERSION_INFO_BUILDER", pw->pw_name);

	/* The banner, with the two spaces and the trailing hyphen. */
	if (product != NULL && *product != '\0' && pname != NULL && *pname != '\0') {
		char banner[PATH_MAX + 128];

		cver = settings_get(t, "CURRENT_PROJECT_VERSION");
		snprintf(banner, sizeof(banner),
		    "\"@(#)PROGRAM:%s  PROJECT:%s-%s\"", product, pname,
		    (cver != NULL) ? cver : "");
		settings_defaults_set(t, "VERSION_INFO_STRING", banner);
	}

	/* Where a bundled XPC service would go.  Same for every product. */
	settings_defaults_set(t, "XPCSERVICES_FOLDER_PATH", "/XPCServices");
}

/*
 * The per-user caches.
 *
 * Two families under the Darwin user cache directory, and the shape is Apple's
 * rather than ours:
 *
 *   <user cache>/com.apple.DeveloperTools/<version>/<product>
 *   <user cache>/org.llvm.clang/ModuleCache.noindex/Session.modulevalidation
 *
 * <version> is the product's own "<short>-<build>" -- 26.6-17F113 for Xcode
 * 26.6 -- and <product> is the bundle's name.  Both come out of the developer
 * directory that was resolved, not out of this tool: it is the tree that is
 * building, the same rule the SYSTEM_DEVELOPER_ family already follows.  The
 * short version is CFBundleShortVersionString and the build is
 * ProductBuildVersion, both from the bundle's version.plist -- and notably not
 * CFBundleVersion, which for Xcode is a different number entirely (24959
 * against 17F113), so the obvious plist key is the wrong one.
 *
 * The SDK stat cache adds a component that looks platform-shaped and is not:
 *
 *   <cache root>/SDKStatCaches.noindex/<canonical>-<sdkbuild>-<md5 of sdkpath>
 *
 * The first is the SDK's own CanonicalName ("macosx26.5") and the last is the
 * MD5 of the resolved SDK directory.  It reads like it wants a platform model
 * and does not: every part comes from the SDK already resolved for -sdk, so it
 * moves to iphoneos26.5 on its own.
 *
 * A developer directory that is not inside a bundle -- CommandLineTools, or a
 * plain directory the caller passed -- has no version.plist to read, and
 * nothing is invented in that case: the versioned paths are left unemitted
 * rather than filled in with a plausible number.
 */
static void derive_cache_paths(settings_table *t, const char *devpath)
{
	static const char dev_suffix[] = "/Contents/Developer";
	char user_cache[PATH_MAX];
	char root[PATH_MAX], buf[PATH_MAX];
	const char *cache;
	char *shortv, *build, *canonical, *sdkbuild;
	const char *sdkroot;
	size_t dlen, clen, blen;

	/*
	 * confstr(3) fills the buffer and returns how many bytes it wrote,
	 * which is not the POSIX signature -- Darwin's returns size_t rather
	 * than a char *, so the pointer-shaped call does not compile.  Zero is
	 * the failure case either way.
	 */
	if (confstr(_CS_DARWIN_USER_CACHE_DIR, user_cache, sizeof(user_cache)) == 0)
		return;
	cache = user_cache;
	clen = strlen(cache);
	while (clen > 1 && cache[clen - 1] == '/')
		clen--;

	/* The clang one needs neither a product version nor an SDK. */
	snprintf(buf, sizeof(buf), "%.*s/org.llvm.clang/ModuleCache.noindex/"
	    "Session.modulevalidation", (int)clen, cache);
	settings_defaults_set(t, "CLANG_MODULES_BUILD_SESSION_FILE", buf);

	dlen = strlen(devpath);
	if (dlen <= sizeof(dev_suffix) - 1 ||
	    strcmp(devpath + dlen - (sizeof(dev_suffix) - 1), dev_suffix) != 0)
		return;

	{
		char vpath[PATH_MAX];
		CFDictionaryRef dict;

		snprintf(vpath, sizeof(vpath), "%.*s/Contents/version.plist",
		    (int)(dlen - (sizeof(dev_suffix) - 1)), devpath);
		if ((dict = cfplist_read(vpath)) == NULL)
			return;
		shortv = cfplist_string(dict, "CFBundleShortVersionString");
		build = cfplist_string(dict, "ProductBuildVersion");
		CFRelease(dict);
	}
	if (shortv == NULL || build == NULL) {
		free(shortv);
		free(build);
		return;
	}

	/* <product> is the enclosing bundle's name, without the extension. */
	{
		char bundle[PATH_MAX];
		const char *base, *slash;

		snprintf(bundle, sizeof(bundle), "%.*s",
		    (int)(dlen - (sizeof(dev_suffix) - 1)), devpath);
		slash = strrchr(bundle, '/');
		base = slash != NULL ? slash + 1 : bundle;
		blen = strlen(base);
		if (blen > 4 && strcmp(base + blen - 4, ".app") == 0)
			blen -= 4;

		snprintf(root, sizeof(root),
		    "%.*s/com.apple.DeveloperTools/%s-%s/%.*s",
		    (int)clen, cache, shortv, build, (int)blen, base);
	}

	free(shortv);
	free(build);

	settings_defaults_set(t, "CACHE_ROOT", root);
	settings_defaults_set(t, "CCHROOT", root);
	settings_defaults_set(t, "SDK_STAT_CACHE_DIR", root);

	snprintf(buf, sizeof(buf), "%s/CompilationCache.noindex", root);
	settings_defaults_set(t, "COMPILATION_CACHE_CAS_PATH", buf);

	sdkroot = settings_get(t, "SDKROOT");
	if (sdkroot == NULL || *sdkroot != '/')
		return;

	canonical = xt_sdk_setting(sdkroot, "CanonicalName");
	sdkbuild = xt_sdk_build_version(sdkroot);
	if (canonical == NULL || sdkbuild == NULL) {
		free(canonical);
		free(sdkbuild);
		return;
	}

	{
		char digest[33];

		md5_hex(sdkroot, strlen(sdkroot), digest);
		snprintf(buf, sizeof(buf), "%s/SDKStatCaches.noindex/%s-%s-%s"
		    ".sdkstatcache", root, canonical, sdkbuild, digest);
		settings_defaults_set(t, "SDK_STAT_CACHE_PATH", buf);
	}

	free(canonical);
	free(sdkbuild);
}

/*
 * The settings a target builds with.
 *
 * `target` is the target to resolve for, which is normally the one the
 * command line asked for -- but a build resolves each dependency in
 * turn, and what a target produces and what it is called are its own.
 */
static settings_table *settings_for(const xcodebuild_opts *opts,
                                    const char *devpath, const char *target,
                                    const char *project_override)
{
	const char *sdkname = xbuild_resolve_sdk_name(opts, devpath);
	const char *tcname = xbuild_resolve_toolchain_name(opts, devpath, sdkname);
	const char *configuration = opts->configuration;
	const char *arch = opts->arch;

	char *project = (project_override != NULL) ? strdup(project_override) :
	                detect_project(opts, opts->project_dir);

	settings_table *t = settings_create();
	if (t == NULL) {
		free(project);
		return NULL;
	}

	/*
	 * Which configuration a build without -configuration uses is the
	 * project's to say, and this is where it is settled.  It used to
	 * be answered "Debug" here while -list reported the project's own
	 * answer, so one tool gave two different defaults for the same
	 * project and a build without -configuration silently came out
	 * Debug.  Apple answers Release, which is also the documented
	 * default, so that stays the fallback here.
	 *
	 * It has to happen before the defaults are loaded: the name goes
	 * into BUILT_PRODUCTS_DIR, the temporary directories and the
	 * CONFIGURATION setting, all of which are computed from it.
	 */
	CFTypeRef root = (project != NULL) ? project_load_pbxproj(project) : NULL;
	char defcfg[128];

	if (configuration == NULL) {
		const char *d = (root != NULL) ? project_default_configuration(root,
		    defcfg, sizeof(defcfg)) : NULL;

		configuration = (d != NULL && *d != '\0') ? d : "Release";
	}

	if (settings_load_defaults(t, devpath, sdkname, tcname, configuration, arch) != 0)
		fprintf(stderr, "xcodebuild: warning: could not load SDK info for '%s'\n", sdkname);

	if (target != NULL)
		settings_set(t, "TARGET_NAME", target);

	if (root != NULL) {
		char chosen[512], pname[512], sr[PATH_MAX];

		/*
		 * The project's own name, and the name of the
		 * target the settings come from.  Both are set
		 * before the merge: the values being merged are
		 * written in terms of them -- PRODUCT_NAME is
		 * $(TARGET_NAME) in almost every project -- and
		 * expand to nothing if they are not there yet.
		 */
		project_display_name(project, pname, sizeof(pname));
		if (pname[0] != '\0')
			settings_set(t, "PROJECT_NAME", pname);

		/*
		 * Where the project lives, as an absolute path.
		 * Paths in its settings are written relative to
		 * this, and $(SRCROOT) appears in them constantly.
		 */
		{
			/*
			 * Absolute, because Apple reports it absolute.
			 * Every path a project writes as $(SRCROOT)/... is
			 * expanded against this, and so is the chain of
			 * build directories below it -- leaving it as "." or
			 * as the .xcodeproj argument verbatim made all
			 * 31 of those relative where Apple's are absolute,
			 * which is the single largest source of differing
			 * lines in -showBuildSettings.  The path need not
			 * exist for this: xc_abspath() resolves "."
			 * and ".." textually rather than touching the
			 * filesystem, so a build directory that has not
			 * been created yet still gets a settled name.
			 */
			char dir[PATH_MAX];

			if (xc_dirname(project, dir, sizeof(dir)) == NULL ||
			    xc_abspath(dir, dir, sizeof(dir)) == NULL)
				snprintf(sr, sizeof(sr), ".");
			else
				snprintf(sr, sizeof(sr), "%s", dir);
			settings_set(t, "SRCROOT", sr);
			settings_set(t, "SOURCE_ROOT", sr);
			settings_set(t, "PROJECT_DIR", sr);

			/*
			 * Where the products go.  A project says
			 * $(BUILT_PRODUCTS_DIR) to find what it has
			 * just built -- a framework it links against
			 * above all -- and the settings it merges
			 * are expanded as they are merged, so this
			 * has to be known before that happens.
			 */
			{
				char bd[PATH_MAX];

				if (opts->build_root != NULL)
					snprintf(bd, sizeof(bd), "%s/%s",
					         opts->build_root,
					         configuration);
				else
					snprintf(bd, sizeof(bd),
					         "%s/build/%s", sr,
					         configuration);
				settings_set(t, "BUILT_PRODUCTS_DIR", bd);
				settings_set(t, "CONFIGURATION_BUILD_DIR", bd);
				settings_set(t, "TARGET_BUILD_DIR", bd);

				/*
				 * FRAMEWORK_SEARCH_PATHS is not seeded
				 * here.  Apple puts the products directory
				 * on the front of whatever the project
				 * wrote, so doing it here -- before the
				 * merge -- would prepend to the seed
				 * rather than to the project, and the
				 * project would never get a say.  It is
				 * done in derive_build_dirs() instead.
				 */
			}

			/*
			 * Where the intermediates go, read back from
			 * Apple: the products under build/, and
			 * everything else under a directory per
			 * project, per configuration and per target.
			 */
			{
				char root_dir[PATH_MAX];
				char proj_dir[PATH_MAX];
				char cfg_dir[PATH_MAX];

				if (opts->build_root != NULL)
					snprintf(root_dir,
					         sizeof(root_dir), "%s",
					         opts->build_root);
				else
					snprintf(root_dir,
					         sizeof(root_dir),
					         "%s/build", sr);
				settings_set(t, "SYMROOT", root_dir);
				settings_set(t, "OBJROOT", root_dir);

				snprintf(proj_dir, sizeof(proj_dir),
				         "%s/%s.build", root_dir,
				         (pname[0] != '\0') ? pname :
				         "project");
				settings_set(t, "PROJECT_TEMP_DIR",
				             proj_dir);

				snprintf(cfg_dir, sizeof(cfg_dir),
				         "%s/%s", proj_dir, configuration);
				settings_set(t, "CONFIGURATION_TEMP_DIR",
				             cfg_dir);

				settings_set(t, "BUILD_DIR", root_dir);
				settings_set(t, "BUILD_ROOT", root_dir);


				/*
				 * Where `install` would put things.
				 * Apple names a directory under /tmp
				 * after the project, and nothing is
				 * written there by a plain build.
				 */
				{
					char dst[PATH_MAX];

					snprintf(dst, sizeof(dst),
					         "/tmp/%s.dst",
					         (pname[0] != '\0') ?
					         pname : "project");
					settings_set(t, "DSTROOT", dst);
					settings_set(t, "INSTALL_ROOT", dst);
				}
			}
		}

		/* Project settings first; a target's inherit them. */
		settings_merge_plist_dict(t,
		    project_find_project_buildsettings(root, configuration));

		CFTypeRef bs = project_find_buildsettings(root,
		    target, configuration, chosen, sizeof(chosen));

		if (target == NULL && chosen[0] != '\0')
			settings_set(t, "TARGET_NAME", chosen);

		if (bs != NULL)
			settings_merge_plist_dict(t, bs);

		{
			char pt[128];

			project_target_product_type(root,
			    (target != NULL) ? target :
			    (chosen[0] != '\0' ? chosen : NULL),
			    pt, sizeof(pt));
			build_apply_product_settings(t, pt);
		}

		/*
		 * Now that the project's own settings are in, settle
		 * where the intermediates go.  A target's directory is
		 * per target so that two targets that each compile a
		 * main.c cannot write the same object, and neither can
		 * tell its leftovers from the other's.
		 *
		 * The command line goes on first.  It is the last word
		 * on any setting, but "last word" has to mean last word
		 * in the *input*, not last thing to happen: deriving
		 * before the overrides land means a
		 * CONFIGURATION_BUILD_DIR= on the command line moves the
		 * products directory and everything that reads it back --
		 * BUILT_PRODUCTS_DIR, the search paths, the signing and
		 * dSYM folders -- keeps reporting where it was before.
		 */
		apply_setting_overrides(t, opts);

		/*
		 * -sdk picks the SDK, so it outranks the SDKROOT the
		 * project wrote.  It used to be used only to seed the
		 * defaults, and the merge then put the project's own
		 * SDKROOT back -- so `-sdk macosx` against a project
		 * that hardcodes MacOSX.Internal.sdk built against the
		 * internal SDK while appearing to ask for another.
		 *
		 * Resolved to a path here, because the block further down
		 * turns a bare name into a path using whichever SDK_DIR
		 * the merge left behind, which is the one -sdk was meant
		 * to replace.
		 */
		if (opts != NULL && opts->sdk != NULL && *opts->sdk != '\0') {
			char *sp = (opts->sdk[0] == '/') ?
			    strdup(opts->sdk) : xt_find_sdk(devpath, opts->sdk);

			if (sp != NULL) {
				settings_set(t, "SDKROOT", sp);
				free(sp);
			}
		}

		derive_build_dirs(t);

		/*
		 * After the merge, because two of the nine are named
		 * after settings the project writes: PRODUCT_NAME is
		 * $(TARGET_NAME) in almost every project, and expanding
		 * it needs the target's own settings in place.
		 */
		derive_identity(t, root, project, target);
	} else if (project != NULL && opts->verbose) {
		fprintf(stderr, "xcodebuild: warning: could not parse project '%s'\n", project);
	}

	if (root != NULL)
		CFRelease(root);
	free(project);

	if (opts->xcconfig != NULL) {
		if (settings_load_xcconfig(t, opts->xcconfig) != 0)
			fprintf(stderr, "xcodebuild: warning: could not read xcconfig '%s'\n", opts->xcconfig);
	}

	/*
	 * The developer directory's own furniture.  These describe the
	 * toolchain in use, so they are this tree's paths rather than
	 * Apple's -- a project asking for $(DEVELOPER_BIN_DIR) wants the
	 * tools that are building it.
	 */
	if (devpath != NULL) {
		static const struct { const char *key, *tail; } d[] = {
			{ "DEVELOPER_USR_DIR",          "/usr" },
			{ "DEVELOPER_BIN_DIR",          "/usr/bin" },
			{ "DEVELOPER_LIBRARY_DIR",      "/Library" },
			{ "DEVELOPER_APPLICATIONS_DIR", "/Applications" },
			{ "DEVELOPER_FRAMEWORKS_DIR",   "/Library/Frameworks" },
			{ "DEVELOPER_FRAMEWORKS_DIR_QUOTED", "/Library/Frameworks" },
			{ "DEVELOPER_TOOLS_DIR",        "/Tools" },
			{ "DEVELOPER_SDK_DIR",
			  "/Platforms/MacOSX.platform/Developer/SDKs" },
			{ "DT_TOOLCHAIN_DIR",
			  "/Toolchains/XcodeDefault.xctoolchain" },

			/* The same furniture again under two prefixes:
			 * SYSTEM_DEVELOPER_ and the per-platform dirs.
			 * SYSTEM_DEVELOPER_ sits beside DEVELOPER_DIR, so
			 * the same tails as DEVELOPER_ -- only the
			 * spelling varies, and not by any rule worth
			 * relying on, since APPS_DIR and
			 * APPLICATIONS_DIR are both /Applications and
			 * TOOLS carries no _DIR at all.  So they are
			 * spelled out rather than derived.
			 *
			 * PLATFORM_DEVELOPER_ moves with the selected
			 * SDK's platform and is derived in
			 * settings.c; on macOS it happens to equal
			 * DEVELOPER_DIR plus the same tail.
			 *
			 * Apple resolves these against the bundle it was
			 * launched from and ignores DEVELOPER_DIR
			 * entirely, even when it names a perfectly good
			 * developer dir: xcode-select honours the
			 * variable, xcodebuild does not.  This tree is
			 * the one building, which is the point of them.
			 */

			{ "SYSTEM_DEVELOPER_DIR",                "" },
			{ "SYSTEM_DEVELOPER_APPS_DIR",           "/Applications" },
			{ "SYSTEM_DEVELOPER_BIN_DIR",            "/usr/bin" },
			{ "SYSTEM_DEVELOPER_USR_DIR",            "/usr" },
			{ "SYSTEM_DEVELOPER_TOOLS",              "/Tools" },
			{ "SYSTEM_DEVELOPER_UTILITIES_DIR",      "/Applications/Utilities" },
			{ "SYSTEM_DEVELOPER_DEMOS_DIR",
			  "/Applications/Utilities/Built Examples" },
			{ "SYSTEM_DEVELOPER_GRAPHICS_TOOLS_DIR",
			  "/Applications/Graphics Tools" },
			{ "SYSTEM_DEVELOPER_JAVA_TOOLS_DIR",     "/Applications/Java Tools" },
			{ "SYSTEM_DEVELOPER_PERFORMANCE_TOOLS_DIR",
			  "/Applications/Performance Tools" },
			{ "SYSTEM_DEVELOPER_DOC_DIR",            "/ADC Reference Library" },
			{ "SYSTEM_DEVELOPER_RELEASENOTES_DIR",
			  "/ADC Reference Library/releasenotes" },
			{ "SYSTEM_DEVELOPER_TOOLS_DOC_DIR",
			  "/ADC Reference Library/documentation/DeveloperTools" },
			{ "SYSTEM_DEVELOPER_TOOLS_RELEASENOTES_DIR",
			  "/ADC Reference Library/releasenotes/DeveloperTools" }
		};
		char buf[PATH_MAX];
		size_t i;

		for (i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
			snprintf(buf, sizeof(buf), "%s%s", devpath, d[i].tail);
			settings_defaults_set(t, d[i].key, buf);
		}
	}

	/*
	 * SDKROOT as a path.  A project writes "macosx" and means
	 * whichever SDK that resolves to; Apple reports the resolved
	 * path, and so does this now that one is known.
	 */
	{
		const char *root = settings_get(t, "SDKROOT");
		const char *dir = settings_get(t, "SDK_DIR");

		if (dir != NULL && *dir == '/' &&
		    (root == NULL || *root != '/'))
			settings_set(t, "SDKROOT", dir);
	}

	/*
	 * With SDKROOT settled, name the SDK it points at.  This has to
	 * follow the block above, which is what turns a name like
	 * "macosx" into a path.
	 */
	settings_sync_sdk_root(t);

	/*
	 * The command line's SETTING=value overrides are the last word
	 * on any setting, so they are applied after everything above --
	 * they used to be applied before the developer directory's paths
	 * and the SDKROOT resolution, which quietly lost an explicit
	 * SDKROOT to whichever SDK the scan had found.
	 *
	 * They go on both sides of the SDK sync above, because an
	 * SDKROOT given here is exactly the case the sync has to follow:
	 * setting SDKROOT= on the command line moves the SDK, and the
	 * identity that comes with it has to move too.  The second pass
	 * is what keeps an explicit SDK_VERSION= from being overruled by
	 * the SDK's own.
	 */
	apply_setting_overrides(t, opts);
	settings_sync_sdk_root(t);
	apply_setting_overrides(t, opts);

	derive_cache_paths(t, devpath);

	settings_set(t, "ACTION", opts->action ? opts->action : "build");
	return t;
}

static settings_table *resolve_settings(const xcodebuild_opts *opts,
                                        const char *devpath)
{
	return settings_for(opts, devpath, opts->target, NULL);
}

settings_table *xbuild_settings_for_target(const xcodebuild_opts *opts,
                                           const char *devpath,
                                           const char *target,
                                           const char *project)
{
	return settings_for(opts, devpath, target, project);
}

/*
 * Build a workspace's scheme.
 *
 * A workspace has no targets of its own; it refers to projects, and a
 * scheme names targets across them.  The scheme is looked for in the
 * workspace first and then in each project, since a project's own
 * schemes are part of a workspace too, and a name matching no file at
 * all is the target of that name -- Xcode creates those on demand and
 * writes nothing down.
 *
 * Every project builds into one directory rather than each into its
 * own, which is the point of a workspace: a target in one project can
 * link what a target in another has just built.
 */
static int
workspace_build(xcodebuild_opts *opts, const char *devpath)
{
	char **projects = NULL, **names = NULL, **containers = NULL;
	char root[PATH_MAX];
	int nprojects, n = 0, i, j, rc = 0, built = 0;

	if (opts->scheme == NULL) {
		fprintf(stderr, "xcodebuild: error: a workspace is built by"
		    " scheme; pass -scheme\n");
		return 1;
	}

	nprojects = workspace_projects(opts->workspace, &projects);
	if (nprojects == 0) {
		fprintf(stderr, "xcodebuild: error: the workspace '%s' refers"
		    " to no projects\n", opts->workspace);
		return 1;
	}

	/* One build directory beside the workspace, shared by them all. */
	if (xc_dirname(opts->workspace, root, sizeof(root)) == NULL)
		snprintf(root, sizeof(root), ".");
	strlcat(root, "/build", sizeof(root));
	opts->build_root = strdup(root);

	/* The scheme: the workspace's own, then any project's. */
	n = project_scheme_targets(opts->workspace, opts->scheme, &names,
	    &containers);

	for (i = 0; n == 0 && i < nprojects; i++)
		n = project_scheme_targets(projects[i], opts->scheme, &names,
		    &containers);

	for (i = 0; i < nprojects && rc == 0; i++) {
		char *only[64];
		int nonly = 0;
		settings_table *t;

		if (n > 0) {
			/*
			 * The entries naming this project.  A container is
			 * written relative to the workspace, so the tail of
			 * the path is what identifies it.
			 */
			for (j = 0; j < n && nonly < 64; j++) {
				const char *c = (containers != NULL) ?
				    containers[j] : NULL;
				const char *tail;

				if (c == NULL || *c == '\0')
					continue;
				if ((tail = strchr(c, ':')) != NULL)
					tail++;
				else
					tail = c;

				if (strlen(projects[i]) >= strlen(tail) &&
				    strcmp(projects[i] + strlen(projects[i]) -
				    strlen(tail), tail) == 0)
					only[nonly++] = names[j];
			}
		} else {
			/* No scheme file: the name is a target's. */
			only[nonly++] = opts->scheme;
		}

		if (nonly == 0)
			continue;

		t = xbuild_settings_for_target(opts, devpath, only[0],
		                               projects[i]);
		if (t == NULL) {
			rc = 1;
			break;
		}

		rc = build_run(projects[i], t, opts, devpath, only, nonly);
		settings_destroy(t);

		/* 2 means the project had no part in this scheme. */
		if (rc == 2)
			rc = 0;
		else if (rc == 0)
			built++;
	}

	if (rc == 0 && built == 0) {
		fprintf(stderr, "xcodebuild: error: scheme '%s' not found in"
		    " the workspace\n", opts->scheme);
		rc = 1;
	}

	if (rc == 0)
		printf("\n** BUILD SUCCEEDED **\n\n");
	else
		printf("\n** BUILD FAILED **\n\n");

	for (i = 0; i < n; i++) {
		free(names[i]);
		if (containers != NULL)
			free(containers[i]);
	}
	free(names);
	free(containers);

	for (i = 0; i < nprojects; i++)
		free(projects[i]);
	free(projects);

	return rc;
}

/* ------------------------------------------------------------------ */
/* Delegation via xcrun (build / test / archive / install actions)    */
/* ------------------------------------------------------------------ */

static char *find_xcrun(const char *devpath)
{
	char candidate[PATH_MAX];
	snprintf(candidate, sizeof(candidate), "%s/usr/bin/xcrun", devpath);
	if (access(candidate, X_OK) == 0)
		return strdup(candidate);
	return strdup("xcrun");
}

/* Populate `envp` (caller-frees entries via the NULL terminator) with the
 * KEY=VALUE strings a delegated build driver expects. */
static void build_envp(settings_table *t, const char *devpath,
                       const char *sdkname, const char *tcname, char *envp[8])
{
	char *tc_parent = path_join(devpath, "Toolchains");
	char tc_full[PATH_MAX];
	snprintf(tc_full, sizeof(tc_full), "%s/%s.toolchain", tc_parent, tcname);
	free(tc_parent);
	char *tc_dir = strdup(tc_full);

	const char *sdkroot = settings_get(t, "SDKROOT");
	char *sdk_root_owned = NULL;
	if (sdkroot == NULL) {
		sdk_root_owned = path_join(devpath, "SDKs");
		sdkroot = sdk_root_owned;
	}

	const char *home = getenv("HOME");
	const char *oldpath = getenv("PATH");
	const char *triple = settings_get(t, "TARGET_TRIPLE");
	const char *deploy = settings_get(t, "MACOSX_DEPLOYMENT_TARGET");
	if (deploy == NULL)
		deploy = settings_get(t, "IOS_DEPLOYMENT_TARGET");

	int i = 0;
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "SDKROOT=%s", sdkroot);
	envp[i++] = strdup(buf);
	snprintf(buf, sizeof(buf), "PATH=%s/usr/bin:%s/usr/bin:%s", devpath, tc_dir, oldpath ? oldpath : "");
	envp[i++] = strdup(buf);
	snprintf(buf, sizeof(buf), "LD_LIBRARY_PATH=%s/usr/lib", tc_dir);
	envp[i++] = strdup(buf);
	snprintf(buf, sizeof(buf), "HOME=%s", home ? home : "");
	envp[i++] = strdup(buf);
	if (triple != NULL) {
		snprintf(buf, sizeof(buf), "TARGET_TRIPLE=%s", triple);
		envp[i++] = strdup(buf);
	}
	if (deploy != NULL) {
		snprintf(buf, sizeof(buf), "MACOSX_DEPLOYMENT_TARGET=%s", deploy);
		envp[i++] = strdup(buf);
	}
	envp[i] = NULL;
	free(tc_dir);
	free(sdk_root_owned);
}

static int exec_build_action(xcodebuild_opts *opts, const char *devpath,
                             const char *sdkname, const char *tcname,
                             settings_table *t, const char *action)
{
	char *xcrun = find_xcrun(devpath);
	const char *tool = action;
	if (strcmp(action, "test-without-building") == 0)
		tool = "test";

	size_t n = opts->n_overrides;
	char **argv = (char **)malloc(sizeof(char *) * (7 + n + 1));
	if (argv == NULL) { free(xcrun); return 1; }
	int j = 0;
	argv[j++] = xcrun;
	argv[j++] = strdup("-sdk");
	argv[j++] = strdup(sdkname);
	argv[j++] = strdup("-toolchain");
	argv[j++] = strdup(tcname);
	argv[j++] = strdup(tool);
	for (size_t i = 0; i < n; i++)
		argv[j++] = strdup(opts->overrides[i]);
	argv[j] = NULL;

	char *envp[8] = {0};
	build_envp(t, devpath, sdkname, tcname, envp);
	for (int k = 0; envp[k] != NULL; k++)
		putenv(envp[k]);

	if (opts->verbose || opts->dry_run) {
		fprintf(stderr, "xcodebuild: %s: \"" , opts->dry_run ? "dry-run" : "verbose");
		fprintf(stderr, "%s", argv[0]);
		for (int k = 1; argv[k] != NULL; k++)
			fprintf(stderr, " %s", argv[k]);
		fputs("\"\n", stderr);
	}

	int rc = 0;
	if (opts->dry_run) {
		rc = 0;
	} else {
		execvp(argv[0], argv);
		fprintf(stderr, "xcodebuild: error: failed to exec '%s': %s\n", argv[0], strerror(errno));
		rc = 1;
	}

	for (int k = 0; argv[k] != NULL; k++)
		free(argv[k]);
	free(argv);
	for (int k = 0; envp[k] != NULL; k++)
		free(envp[k]);
	return rc;
}

/* ------------------------------------------------------------------ */
/* clean                                                               */
/* ------------------------------------------------------------------ */

static int do_clean(const xcodebuild_opts *opts, settings_table *t)
{
	const char *products = settings_get(t, "BUILT_PRODUCTS_DIR");
	const char *build_dir = settings_get(t, "BUILD_DIR");
	const char *config_temp = settings_get(t, "CONFIGURATION_TEMP_DIR");

	if (opts->verbose)
		fprintf(stderr, "xcodebuild: cleaning build products\n");

	if (products != NULL && *products != '\0') {
		if (opts->dry_run)
			fprintf(stderr, "xcodebuild: rm -rf %s\n", products);
		else
			rmtree(products);
	}
	if (build_dir != NULL && *build_dir != '\0' && build_dir != products) {
		if (opts->dry_run)
			fprintf(stderr, "xcodebuild: rm -rf %s\n", build_dir);
		else
			rmtree(build_dir);
	}
	if (config_temp != NULL && *config_temp != '\0') {
		if (opts->dry_run)
			fprintf(stderr, "xcodebuild: rm -rf %s\n", config_temp);
		else
			rmtree(config_temp);
	}
	if (!opts->quiet)
		fprintf(stdout, "xcodebuild: clean complete\n");
	return 0;
}

static int do_export_archive(const xcodebuild_opts *opts)
{
	if (opts->archive_path == NULL || opts->export_path == NULL || opts->export_options_plist == NULL) {
		fprintf(stderr, "xcodebuild: error: -exportArchive requires -archivePath, -exportPath and -exportOptionsPlist\n");
		return 1;
	}

	CFDictionaryRef options = cfplist_read(opts->export_options_plist);
	if (options == NULL) {
		fprintf(stderr, "xcodebuild: error: cannot read export options plist '%s'\n",
		        opts->export_options_plist);
		return 1;
	}

	char *method = cfplist_string(options, "method");
	char *destination = cfplist_string(options, "destination");
	char *team = cfplist_string(options, "teamID");
	CFRelease(options);

	if (method == NULL) {
		fprintf(stderr, "xcodebuild: error: exportOptions.plist does not specify a 'method'\n");
		free(method); free(destination); free(team);
		return 1;
	}

	if (opts->verbose)
		fprintf(stderr, "xcodebuild: exporting archive '%s' -> '%s' (method=%s, destination=%s%s%s)\n",
		        opts->archive_path, opts->export_path, method,
		        destination ? destination : "not specified",
		        team ? ", team=" : "", team ? team : "");

	free(method);
	free(destination);
	free(team);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static char *consume_value(int *i, int argc, char **argv, const char *val)
{
	if (val != NULL)
		return strdup(val);
	if (*i + 1 >= argc) {
		fprintf(stderr, "xcodebuild: error: option requires an argument\n");
		exit(1);
	}
	(*i)++;
	return strdup(argv[*i]);
}

static void set_opt(char **slot, const char *val)
{
	free(*slot);
	*slot = strdup(val ? val : "");
}

static xcodebuild_opts *parse_args(int argc, char **argv)
{
	xcodebuild_opts *opts = xbuild_opts_create();
	if (opts == NULL)
		return NULL;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (arg[0] != '-' || arg[1] == '\0') {
			if (arg[0] == '-' && arg[1] == '\0')
				continue; /* "--" skip */
			if (opts->action == NULL && is_action(arg))
				opts->action = strdup(arg);
			else if (strchr(arg, '=') != NULL)
				xbuild_opt_add_override(opts, arg);
			continue;
		}

		char keybuf[64];
		char *eq = strchr(argv[i], '=');
		const char *key;
		const char *val = NULL;
		if (eq != NULL) {
			size_t kl = (size_t)(eq - argv[i]);
			if (kl >= sizeof(keybuf)) kl = sizeof(keybuf) - 1;
			memcpy(keybuf, argv[i], kl);
			keybuf[kl] = '\0';
			key = keybuf;
			val = eq + 1;
		} else {
			key = argv[i];
		}

		if (strcmp(key, "-h") == 0 || strcmp(key, "-help") == 0 || strcmp(key, "--help") == 0)
			opts->help = 1;
		else if (strcmp(key, "-version") == 0 || strcmp(key, "--version") == 0)
			opts->version = 1;
		else if (strcmp(key, "-list") == 0)
			opts->list_targets = 1;
		else if (strcmp(key, "-showBuildSettings") == 0)
			opts->show_build_settings = 1;
		else if (strcmp(key, "-showsdks") == 0)
			opts->show_sdks = 1;
		else if (strcmp(key, "-showBuildableProducts") == 0)
			opts->show_buildable_products = 1;
		else if (strcmp(key, "-showRuntimeSearchable") == 0)
			opts->show_runtime_searchable = 1;
		else if (strcmp(key, "-exportArchive") == 0)
			opts->export_archive = 1;
		else if (strcmp(key, "-alltargets") == 0)
			opts->all_targets = 1;
		else if (strcmp(key, "-parallelizeTargets") == 0)
			opts->parallel_targets = 1;
		else if (strcmp(key, "-json") == 0)
			opts->json = 1;
		else if (strcmp(key, "-pretty") == 0)
			opts->pretty = 1;
		else if (strcmp(key, "-quiet") == 0)
			opts->quiet = 1;
		else if (strcmp(key, "-dry-run") == 0)
			opts->dry_run = 1;
		else if (strcmp(key, "-verbose") == 0 || strcmp(key, "-v") == 0)
			opts->verbose = 1;
		else if (strcmp(key, "-allowProvisioningUpdates") == 0)
			opts->allow_provisioning_updates = 1;
		else if (strcmp(key, "-allowProvisioningDeviceRegistration") == 0)
			opts->allow_provisioning_device_registration = 1;
		else if (strcmp(key, "-kill-tests") == 0)
			opts->kill_tests = 1;
		else if (strcmp(key, "-project") == 0)
			set_opt(&opts->project, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-workspace") == 0)
			set_opt(&opts->workspace, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-scheme") == 0)
			set_opt(&opts->scheme, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-target") == 0)
			set_opt(&opts->target, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-configuration") == 0)
			set_opt(&opts->configuration, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-sdk") == 0)
			set_opt(&opts->sdk, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-arch") == 0)
			set_opt(&opts->arch, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-toolchain") == 0)
			set_opt(&opts->toolchain, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-destination") == 0)
			set_opt(&opts->destination, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-xcconfig") == 0)
			set_opt(&opts->xcconfig, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-derivedDataPath") == 0)
			set_opt(&opts->derived_data_path, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-archivePath") == 0)
			set_opt(&opts->archive_path, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-exportPath") == 0)
			set_opt(&opts->export_path, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-exportOptionsPlist") == 0)
			set_opt(&opts->export_options_plist, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-projectDir") == 0)
			set_opt(&opts->project_dir, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-resultBundlePath") == 0)
			set_opt(&opts->result_bundle_path, consume_value(&i, argc, argv, val));
		else if (strcmp(key, "-jobs") == 0) {
			const char *jv = val ? val : (i + 1 < argc ? argv[++i] : NULL);
			opts->jobs = jv ? atoi(jv) : 0;
		} else if (opts->verbose) {
			fprintf(stderr, "xcodebuild: warning: ignoring unknown option '%s'\n", key);
		}
	}

	return opts;
}

/* ------------------------------------------------------------------ */
/* Loader hand-off                                                     */
/* ------------------------------------------------------------------ */

/*
 * Apple's xcodebuild carries the literal string @rpath/libxcodebuildLoader.dylib
 * and calls dlopen/dlsym on it; it neither links the loader nor imports
 * XcodeBuildMain.  This mirrors that.  The run path entry that makes the
 * @rpath resolve is the same ../../../Frameworks one Apple uses, so a staged
 * Contents/Developer/usr/bin/xcodebuild finds Contents/Frameworks.
 *
 * Everything here is best effort.  The loader is a second product, and a tool
 * that has not been installed next to its dylib must still build, so a failure
 * to open it is silent unless the caller asked for verbose output.
 */
static void
loader_preflight(int argc, char **argv, int verbose)
{
	static const char *const path = "@rpath/libxcodebuildLoader.dylib";
	typedef Boolean (*xcode_build_main_fn)(Boolean, CFStringRef, CFStringRef,
	    CFStringRef, CFStringRef);
	typedef void (*xcode_build_set_invocation_fn)(int, char * const *);
	xcode_build_set_invocation_fn set_invocation;
	xcode_build_main_fn entry;
	CFStringRef name;
	CFStringRef bundleID;
	void *handle;

	handle = dlopen(path, RTLD_LAZY);
	if (handle == NULL) {
		if (verbose)
			fprintf(stderr, "xcodebuild: note: %s not loaded: %s\n", path,
			    dlerror());
		return;
	}

	/*
	 * The loader needs our argv to replay on relaunch, and it cannot recover
	 * argv itself, so this must be established before the relaunch is offered.
	 */
	set_invocation = (xcode_build_set_invocation_fn)dlsym(handle,
	    "XcodeBuildSetInvocation");
	entry = (xcode_build_main_fn)dlsym(handle, "XcodeBuildMain");
	if (set_invocation == NULL || entry == NULL) {
		if (verbose)
			fprintf(stderr, "xcodebuild: note: %s lacks the expected "
			    "entry points\n", path);
		dlclose(handle);
		return;
	}

	set_invocation(argc, argv);

	name = CFStringCreateWithCString(kCFAllocatorDefault, "xcodebuild",
	    kCFStringEncodingUTF8);
	bundleID = CFBundleGetIdentifier(CFBundleGetMainBundle());
	entry(true, name, bundleID, name, NULL);
	CFRelease(name);
}

/* ------------------------------------------------------------------ */
/* Main dispatch                                                       */
/* ------------------------------------------------------------------ */

/*
 * Apple opens the console output by echoing the invocation it was handed, so
 * the first two lines of every build, -list or -showBuildSettings are the
 * command that produced the rest.  An argument is quoted when it is not made
 * only of characters a shell passes through unquoted -- alphanumerics and
 * -_./=,+, measured one character at a time against the real tool -- and a
 * backslash or a double quote inside it gains a backslash of its own.  An
 * empty argument is not quoted: it contributes nothing but the separator that
 * precedes it, which is why a trailing "" leaves a trailing space.
 *
 * The echo is skipped for -quiet and -json, and -json implies -quiet, so the
 * JSON is not interleaved with anything meant for a person to read.
 */
static int arg_needs_quoting(const char *s)
{
	for (const char *p = s; *p != '\0'; p++) {
		if (isalnum((unsigned char)*p))
			continue;
		if (strchr("-_./=,+", *p) != NULL)
			continue;
		return 1;
	}
	return 0;
}

static void print_invocation_arg(const char *s)
{
	if (!arg_needs_quoting(s)) {
		fputs(s, stdout);
		return;
	}
	putchar('"');
	for (const char *p = s; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			putchar('\\');
		putchar(*p);
	}
	putchar('"');
}

static void print_invocation(int argc, char **argv)
{
	fputs("Command line invocation:\n    ", stdout);
	for (int i = 0; i < argc; i++) {
		if (i > 0)
			putchar(' ');
		print_invocation_arg(argv[i]);
	}
	putchar('\n');
	putchar('\n');
}

int main(int argc, char **argv)
{
	xcodebuild_opts *opts = parse_args(argc, argv);
	if (opts == NULL)
		return 1;

	if (opts->help)
		usage(stdout, 0);
	if (argc < 2)
		usage(stderr, 1);
	if (opts->version) {
		version_print(opts->verbose);
		xbuild_opts_free(opts);
		return 0;
	}

	char *devpath = xbuild_get_developer_path();
	if (devpath == NULL) {
		xbuild_opts_free(opts);
		return 1;
	}

	if (opts->show_sdks) {
		project_show_sdks(devpath);
		free(devpath);
		xbuild_opts_free(opts);
		return 0;
	}

	/*
	 * Everything below this line is a project-scoped action, and Apple
	 * echoes the invocation for all of them.  The ones above it --
	 * -help, -version, -showsdks -- do not, and neither does an option
	 * that failed to parse, since parse_args has already returned by now.
	 */
	if (!opts->quiet && !opts->json)
		print_invocation(argc, argv);

	if (opts->list_targets) {
		char *project = detect_project(opts, opts->project_dir);
		int r = project_list(project, opts->workspace, opts);
		free(project);
		free(devpath);
		xbuild_opts_free(opts);
		return r;
	}

	if (opts->show_build_settings) {
		settings_table *t = resolve_settings(opts, devpath);
		if (t == NULL) {
			free(devpath);
			xbuild_opts_free(opts);
			return 1;
		}
		int r = settings_emit(t, opts->json, opts->pretty);
		settings_destroy(t);
		free(devpath);
		xbuild_opts_free(opts);
		return r ? 1 : 0;
	}

	if (opts->show_buildable_products) {
		settings_table *t = resolve_settings(opts, devpath);
		if (t == NULL) {
			free(devpath);
			xbuild_opts_free(opts);
			return 1;
		}
		const char *name = settings_get(t, "PRODUCT_NAME");
		if (name != NULL && *name != '\0')
			printf("    %s\n", name);
		settings_destroy(t);
		free(devpath);
		xbuild_opts_free(opts);
		return 0;
	}

	if (opts->export_archive) {
		int r = do_export_archive(opts);
		free(devpath);
		xbuild_opts_free(opts);
		return r;
	}

	const char *action = opts->action ? opts->action : "build";

	if (strcmp(action, "clean") == 0) {
		settings_table *t = resolve_settings(opts, devpath);
		if (t == NULL) {
			free(devpath);
			xbuild_opts_free(opts);
			return 1;
		}
		int r = do_clean(opts, t);
		settings_destroy(t);
		free(devpath);
		xbuild_opts_free(opts);
		return r;
	}

	/*
	 * Offer the AddressSanitizer relaunch only once the tool is committed to
	 * compiling something.  Re-exec is not free, and a --version or -help
	 * invocation has nothing to instrument, so those paths above return first.
	 * On a relaunch this call does not return: the process image is replaced.
	 */
	loader_preflight(argc, argv, opts->verbose);

	settings_table *t = resolve_settings(opts, devpath);
	if (t == NULL) {
		free(devpath);
		xbuild_opts_free(opts);
		return 1;
	}
	const char *sdkname = xbuild_resolve_sdk_name(opts, devpath);
	const char *tcname = xbuild_resolve_toolchain_name(opts, devpath, sdkname);
	int r;

	/*
	 * build compiles the target here.  Everything else still goes to
	 * the delegated driver, which is honest about not existing --
	 * "build" went there too and was looked up as though it were a
	 * tool of that name, so nothing was ever built.
	 */
	if (strcmp(action, "build") == 0 && opts->workspace != NULL) {
		r = workspace_build(opts, devpath);
	} else if (strcmp(action, "build") == 0) {
		char *project = detect_project(opts, opts->project_dir);

		r = build_run(project, t, opts, devpath, NULL, 0);
		free(project);
	} else {
		r = exec_build_action(opts, devpath, sdkname, tcname, t, action);
	}
	settings_destroy(t);
	free(devpath);
	xbuild_opts_free(opts);
	return r;
}
