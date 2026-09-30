/* xcodebuild -- open source reimplementation of Apple's xcodebuild utility
 *
 * -showBuildSettingsForIndex: the compile settings of every source file a
 * target compiles, in the shape the index service consumes.
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

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFString.h>

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cfplist.h"
#include "project.h"
#include "xcodebuild.h"
#include "xcpath.h"

/* ------------------------------------------------------------------ */
/* Fixed parts of the argument vector                                  */
/* ------------------------------------------------------------------ */

/*
 * The index service's own flags, in Apple's order, which no build setting
 * reaches: the warning switches Xcode turns off for its own front end, the
 * strict-aliasing pair, and the record-keeping switches that let it
 * reconstruct a translation unit without running the compiler's back end.
 *
 * They are written out because there is no setting that produces them -- not
 * even a negative one, since the GCC_WARN_* settings name a subset of a list
 * this is not -- so deriving them would mean deriving nothing.
 */
static const char *const kDiagnostics[] = {
	"-fmessage-length=0",
	"-fdiagnostics-show-note-include-stack",
	"-fmacro-backtrace-limit=0",
	"-fno-color-diagnostics",
	"-Wno-trigraphs",
	"-Wno-missing-field-initializers",
	"-Wno-missing-prototypes",
	"-Wno-return-type",
	"-Wno-missing-braces",
	"-Wparentheses",
	"-Wswitch",
	"-Wno-unused-function",
	"-Wno-unused-label",
	"-Wno-unused-parameter",
	"-Wno-unused-variable",
	"-Wunused-value",
	"-Wno-empty-body",
	"-Wno-uninitialized",
	"-Wno-unknown-pragmas",
	"-Wno-shadow",
	"-Wno-four-char-constants",
	"-Wno-conversion",
	"-Wno-constant-conversion",
	"-Wno-int-conversion",
	"-Wno-bool-conversion",
	"-Wno-enum-conversion",
	"-Wno-float-conversion",
	"-Wno-non-literal-null-conversion",
	"-Wno-objc-literal-conversion",
	"-Wshorten-64-to-32",
	"-Wpointer-sign",
	"-Wno-newline-eof",
	"-Wno-implicit-fallthrough",
};

static const char *const kAfterSysroot[] = {
	"-fstrict-aliasing",
	"-Wdeprecated-declarations",
	"-Wno-sign-conversion",
	"-Wno-infinite-recursion",
	"-Wno-comma",
	"-Wno-block-capture-autoreleasing",
	"-Wno-strict-prototypes",
	"-Wno-semicolon-before-method-body",
};

static const char *const kTail[] = {
	"-fretain-comments-from-system-headers",
	"-Xclang",
	"-detailed-preprocessing-record",
	"-Xclang",
	"-fmodule-format=raw",
	"-Xclang",
	"-fallow-pch-with-compiler-errors",
	"-Wno-non-modular-include-in-framework-module",
	"-Wno-incomplete-umbrella",
	"-fmodules-validate-system-headers",
};

/* The two toolchains every record names, in Apple's order. */
static const char *const kToolchains[] = {
	"com.apple.dt.toolchain.Metal.32023.883",
	"com.apple.dt.toolchain.XcodeDefault",
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
	char **v;
	size_t n;
	size_t cap;
} strvec;

static void sv_push(strvec *s, const char *str)
{
	if (str == NULL)
		return;
	if (s->n == s->cap) {
		size_t cap = (s->cap != 0) ? s->cap * 2 : 32;
		char **grown = (char **)realloc(s->v, cap * sizeof(*grown));

		if (grown == NULL)
			return;
		s->v = grown;
		s->cap = cap;
	}
	s->v[s->n++] = strdup(str);
}

static void sv_pushf(strvec *s, const char *fmt, ...)
{
	char buf[PATH_MAX * 2];
	va_list ap;

	if (fmt == NULL)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sv_push(s, buf);
}

static void sv_free(strvec *s)
{
	for (size_t i = 0; i < s->n; i++)
		free(s->v[i]);
	free(s->v);
	memset(s, 0, sizeof(*s));
}

static int sv_has(const strvec *s, const char *str)
{
	for (size_t i = 0; i < s->n; i++) {
		if (strcmp(s->v[i], str) == 0)
			return 1;
	}
	return 0;
}

/* A setting's expanded value, or a copy of the fallback when the setting is
 * absent.  settings_expand's own answer is always released. */
static char *value_of(const settings_table *t, const char *key,
    const char *fallback)
{
	const char *raw = settings_get(t, key);
	char *expanded = (raw != NULL) ? settings_expand(t, raw) : NULL;

	if (expanded != NULL && *expanded != '\0')
		return expanded;
	free(expanded);
	return strdup((fallback != NULL) ? fallback : "");
}

static int is_yes(const char *s)
{
	return s != NULL && (strcmp(s, "YES") == 0 || strcmp(s, "YES_") == 0 ||
	    strcmp(s, "true") == 0 || strcmp(s, "1") == 0);
}

/* A dylib or bundle, as MACH_O_TYPE names them. */
static int is_library_product(const settings_table *t)
{
	char *mach_o = value_of(t, "MACH_O_TYPE", "");
	int lib = strcmp(mach_o, "mh_dylib") == 0 ||
	    strcmp(mach_o, "mh_bundle") == 0;

	free(mach_o);
	return lib;
}

/* The first whitespace-delimited word of a list-valued setting. */
static char *first_word(const char *s)
{
	char buf[128];
	size_t i = 0;

	if (s == NULL)
		return strdup("");
	while (*s == ' ' || *s == '\t')
		s++;
	while (*s != '\0' && *s != ' ' && *s != '\t') {
		if (i + 1 < sizeof(buf))
			buf[i++] = *s;
		s++;
	}
	buf[i] = '\0';
	return strdup(buf);
}

/* Split a list-valued setting into its words. */
static void push_words(strvec *out, const char *list)
{
	char *copy, *word, *save = NULL;

	if (list == NULL || *list == '\0')
		return;
	copy = strdup(list);
	if (copy == NULL)
		return;
	for (word = strtok_r(copy, " \t", &save); word != NULL;
	    word = strtok_r(NULL, " \t", &save))
		sv_push(out, word);
	free(copy);
}

/* "main.c" -> "main.o": the last extension is replaced rather than appended
 * to, so "foo.bar.c" becomes "foo.bar.o" and not "foo.bar.c.o". */
static void object_name(const char *source, char *buf, size_t len)
{
	const char *base = strrchr(source, '/');
	const char *dot;

	base = (base != NULL) ? base + 1 : source;
	dot = strrchr(base, '.');
	if (dot != NULL && dot != base)
		snprintf(buf, len, "%.*s.o", (int)(dot - base), base);
	else
		snprintf(buf, len, "%s.o", base);
}

/* ------------------------------------------------------------------ */
/* The index build's own record                                        */
/* ------------------------------------------------------------------ */

/*
 * What an index build wrote down about itself.
 *
 * -showBuildSettingsForIndex ignores -configuration and any setting given on
 * the command line, and says so nowhere: the Sources fixture and IDETools
 * take the same command line and both name Release as their default
 * configuration, yet the fixture answers Debug and IDETools answers Release.
 * The difference is the shared scheme, whose LaunchAction says Debug, so
 * the scheme is what the configuration is read from and the record is not
 * consulted for it -- a project that has been indexed once still follows
 * its scheme.
 *
 * What the record is read for is the arena: it carries the data-store path
 * of a build that really did index, and it is believed in preference to a
 * computed one when it has one.  It is not what decides whether this build
 * indexes at all -- see index_store_enabled() -- because a record is only
 * there once something has indexed, and asking is what one does before that.
 */
typedef struct {
	char *configuration;	/* the configuration the index build used */
	char *index_store;	/* the arena's data store, or NULL */
	int index_data_store;	/* whether the arena kept one */
} index_record;

/*
 * A reader for the four leaves wanted out of Xcode's JSON, not a parser.
 *
 * These files are machine-written with one key per line, and every value
 * wanted here is a scalar or one nested object, so finding a key and stopping
 * at its value is enough.  A parser would be a few hundred more lines with
 * nothing to catch a mistake in the handful of steps below.
 */
static const char *json_value(const char *obj, const char *key)
{
	char pattern[128];
	const char *p;

	if (obj == NULL)
		return NULL;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	if ((p = strstr(obj, pattern)) == NULL)
		return NULL;
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != ':')
		return NULL;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* The string at `p`, copied out.  Unescapes \\ \" \n \t \r and leaves the
 * rest alone, which covers every character Xcode writes into a path or a
 * configuration name.  Copied by hand: the unescaped text is never longer
 * than the escaped text, so one malloc holds it, and no encoding question
 * arises for the ASCII these files are made of. */
static char *json_string(const char *p)
{
	const char *start;
	char *out, *o;

	if (p == NULL || *p != '"')
		return NULL;
	start = ++p;
	if ((out = malloc(strlen(start) + 1)) == NULL)
		return NULL;

	for (o = out; *p != '\0' && *p != '"'; p++) {
		if (*p != '\\') {
			*o++ = *p;
			continue;
		}
		switch (*++p) {
		case 'n':  *o++ = '\n'; break;
		case 't':  *o++ = '\t'; break;
		case 'r':  *o++ = '\r'; break;
		case '\0':	/* a backslash at the end: nothing left to escape */
			*o = '\0';
			return out;
		default:   *o++ = *p; break;	/* \\ and \" are themselves */
		}
	}
	*o = '\0';
	return out;
}

static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *buf;
	long len;

	if (fp == NULL)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	if ((len = ftell(fp)) < 0 || len > (1 << 20)) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	if ((buf = (char *)malloc((size_t)len + 1)) == NULL) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		free(buf);
		fclose(fp);
		return NULL;
	}
	buf[len] = '\0';
	fclose(fp);
	return buf;
}

/*
 * The DerivedData directory belonging to one project.
 *
 * Each directory's info.plist records the workspace path it was made for, so
 * the belonging is read rather than computed.  That is deliberate: the
 * directory's own name ends in a hash of that path, and although derived_
 * data_leaf() below can compute that name, the workspace path is the one
 * thing both sides agree on, and a project indexed through a workspace is
 * filed under the workspace rather than under itself.  The hash is used
 * where a name has to be produced for a directory that does not exist yet,
 * which is the common case here and the one no info.plist can answer.
 */
static char *find_derived_data(const char *root, const char *project)
{
	DIR *dir;
	struct dirent *ent;
	char *found = NULL;

	if ((dir = opendir(root)) == NULL)
		return NULL;

	while (found == NULL && (ent = readdir(dir)) != NULL) {
		char info[PATH_MAX];
		CFDictionaryRef dict;
		char *owner;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(info, sizeof(info), "%s/%s/info.plist", root,
		    ent->d_name);
		if ((dict = cfplist_read(info)) == NULL)
			continue;
		if ((owner = cfplist_string(dict, "WorkspacePath")) != NULL &&
		    strcmp(owner, project) == 0) {
			snprintf(info, sizeof(info), "%s/%s", root, ent->d_name);
			found = strdup(info);
		}
		free(owner);
		CFRelease(dict);
	}
	closedir(dir);
	return found;
}

static char *derived_data_root(const xcodebuild_opts *opts)
{
	const char *home = getenv("HOME");
	char buf[PATH_MAX];

	if (opts->derived_data_path != NULL &&
	    *opts->derived_data_path != '\0')
		return strdup(opts->derived_data_path);
	if (home == NULL || *home == '\0')
		return NULL;
	snprintf(buf, sizeof(buf), "%s/Library/Developer/Xcode/DerivedData",
	    home);
	return strdup(buf);
}

/* Read the record, if this project has one.  Silent when it has not: a
 * project that was never indexed is the ordinary case, not an error. */
static void index_record_load(const xcodebuild_opts *opts, const char *project,
    index_record *rec)
{
	char path[PATH_MAX];
	char *root, *dd, *text, *id;
	const char *ids;

	memset(rec, 0, sizeof(*rec));
	if (project == NULL || (root = derived_data_root(opts)) == NULL)
		return;
	if ((dd = find_derived_data(root, project)) == NULL) {
		free(root);
		return;
	}
	free(root);

	/* build-description.json names the xcbuilddata directory the last build
	 * wrote; its IDs are a list, and the first is the one in use. */
	snprintf(path, sizeof(path),
	    "%s/Index.noindex/build-description.json", dd);
	if ((text = read_file(path)) == NULL) {
		free(dd);
		return;
	}
	ids = json_value(text, "IDs");
	id = json_string((ids != NULL && *ids == '[') ?
	    strchr(ids, '"') : ids);
	free(text);
	if (id == NULL) {
		free(dd);
		return;
	}

	/* The directory is the ID with ".xcbuilddata" on the end; the suffix is
	 * in the name on disk but not in the ID itself. */
	snprintf(path, sizeof(path), "%s/Index.noindex/Build/Intermediates.noindex"
	    "/XCBuildData/%s.xcbuilddata/build-request.json", dd, id);
	free(id);
	free(dd);
	if ((text = read_file(path)) == NULL)
		return;

	{
		const char *params = json_value(text, "parameters");
		const char *arena = json_value(params, "arenaInfo");
		const char *flag = json_value(arena, "indexEnableDataStore");

		rec->configuration = json_string(json_value(params,
		    "configurationName"));
		rec->index_data_store = (flag != NULL &&
		    strncmp(flag, "true", 4) == 0);
		rec->index_store = json_string(json_value(arena,
		    "indexDataStoreFolderPath"));
	}
	free(text);
}

static void index_record_free(index_record *rec)
{
	free(rec->configuration);
	free(rec->index_store);
	memset(rec, 0, sizeof(*rec));
}

/* ------------------------------------------------------------------ */
/* Where the index store is                                            */
/* ------------------------------------------------------------------ */

/*
 * The name Xcode gives a project's DerivedData directory:
 *
 *	<ProjectName>-<28 letters>
 *
 * The 28 letters are a function of the project's path and of nothing else,
 * which is why the directory turns up before anything has been built there:
 * the name is computed, not left over.  The function is the one in
 * +[NSString dvt_stringWithUniqueFileNameSuffixForDistinguishingStrings:]
 * (DVTFoundation), read out of the disassembly and checked against the
 * directories Xcode actually makes:
 *
 *	digest = MD5(the project's path, UTF-8)
 *	suffix = base26(digest[0..8]) ++ base26(digest[8..16])
 *
 * where each half is read as one big-endian 64-bit number and written as
 * exactly 14 letters, most significant first, 'a' for zero.  Two details
 * make it worth stating, because both are ways to be nearly right:
 *
 *   - the two halves are rendered independently, so this is not the 128-bit
 *     digest written in base 26.  That would be 28 letters too, and the two
 *     differ in every position.
 *   - each half is 14 letters wide whether or not it needs them, so a small
 *     digest still yields leading 'a's.  Nothing is trimmed.
 *
 * The path hashed is the project's settled path, xc_canonpath()'s: symlinks
 * followed and a leading /private off, so that the link, the /private form
 * and the short form of one place all give one directory.  It is the same
 * settling that names the project's SRCROOT, which is why the arena and the
 * paths inside the records that point at it agree.  A project that cannot be
 * settled is hashed as it was named, since a name is better than none and
 * the project is still the one that was asked for.
 */
static char *derived_data_leaf(const char *project)
{
	uint8_t digest[16];
	char suffix[29];
	char canon[PATH_MAX];
	const char *base, *dot, *path;
	char *out;
	size_t len;
	int half, digit;

	if (project == NULL || *project == '\0')
		return NULL;
	path = xc_canonpath(project, canon, sizeof(canon));
	if (path == NULL)
		path = project;
	base = strrchr(path, '/');
	base = (base != NULL) ? base + 1 : path;
	/*
	 * Only the last extension goes: My.Proj.xcodeproj is filed as
	 * My.Proj-<letters>, not as My-<letters>.  The hash covers the whole
	 * path either way, so only the name in front of it moves.
	 */
	dot = strrchr(base, '.');
	if (dot == NULL || dot == base)
		dot = base + strlen(base);

	md5_digest(path, strlen(path), digest);
	for (half = 0; half < 2; half++) {
		uint64_t v = 0;

		for (digit = 0; digit < 8; digit++)
			v = (v << 8) | digest[half * 8 + digit];
		for (digit = 13; digit >= 0; digit--) {
			suffix[half * 14 + digit] = (char)('a' + (v % 26));
			v /= 26;
		}
	}
	suffix[28] = '\0';

	len = (size_t)(dot - base) + 1 + 28 + 1;
	if ((out = malloc(len)) == NULL)
		return NULL;
	snprintf(out, len, "%.*s-%s", (int)(dot - base), base, suffix);
	return out;
}

static char *index_store_path(const xcodebuild_opts *opts, const char *project)
{
	static const char tail[] = "/Index.noindex/DataStore";
	char *root, *leaf, *out;
	size_t len;

	if ((root = derived_data_root(opts)) == NULL)
		return NULL;
	/*
	 * The per-project directory only exists under the default root.  A
	 * -derivedDataPath is taken as it stands -- one store for whatever is
	 * built into it, with no project-named level in between -- so naming
	 * the leaf here would put the store one level too deep for it to be
	 * found, and nothing would ever be written to it.
	 */
	leaf = (opts->derived_data_path != NULL &&
	    *opts->derived_data_path != '\0') ? NULL : derived_data_leaf(project);
	if (leaf == NULL && (opts->derived_data_path == NULL ||
	    *opts->derived_data_path == '\0')) {
		free(root);
		return NULL;
	}
	len = strlen(root) + (leaf != NULL ? 1 + strlen(leaf) : 0) + sizeof(tail);
	if ((out = malloc(len)) == NULL)
		out = NULL;
	else if (leaf != NULL)
		snprintf(out, len, "%s/%s%s", root, leaf, tail);
	else
		snprintf(out, len, "%s%s", root, tail);
	free(root);
	free(leaf);
	return out;
}

/*
 * Whether this build would write an index store at all.
 *
 * This is the question that decides both of the index arguments, and it is
 * not the question the record answers.  A record only exists once something
 * has indexed the project, so gating on it reports no index store for every
 * project that has not been indexed yet -- which is the state a caller
 * asking for these settings is normally in, and the reason the arguments
 * were missing.  What actually decides it is COMPILER_INDEX_STORE_ENABLE,
 * which Xcode does not report in this output and which defaults to on for
 * Debug and off for every other configuration.  A project that states it
 * says otherwise, and is obeyed:
 *
 *	Debug    + unset   ->  indexed
 *	Release  + unset   ->  not indexed
 *	Debug    + NO      ->  not indexed
 *	Release  + YES     ->  indexed
 *
 * The configuration is the one the build resolves to, so the fixture that
 * takes Debug from its scheme indexes and the Release project does not,
 * from one rule rather than two.
 */
static int index_store_enabled(const settings_table *t, const char *configuration)
{
	char *stated = value_of(t, "COMPILER_INDEX_STORE_ENABLE", "");
	int on;

	/*
	 * "Default" is the word this setting carries when nothing has asked for
	 * it, and it is not an answer -- it is the absence of one, so the
	 * compiler goes on to decide by configuration.  Any other word is a real
	 * decision and is taken at face value, including the ones that are
	 * neither YES nor NO.
	 */
	if (*stated != '\0' && strcmp(stated, "Default") != 0) {
		on = is_yes(stated);
		free(stated);
		return on;
	}
	free(stated);
	return configuration != NULL && strcmp(configuration, "Debug") == 0;
}

/*
 * The arena this build indexes into, or NULL when it indexes nowhere.
 *
 * A record that carries a data store is believed, since that is a path
 * something really did write to.  Otherwise the directory is named as above
 * -- it does not have to exist for its name to be the right one, and the
 * path is only ever reported, not opened.
 */
static char *index_store_for(const xcodebuild_opts *opts,
    const settings_table *t, const char *project, const char *configuration,
    const index_record *rec)
{
	if (!index_store_enabled(t, configuration))
		return NULL;
	if (rec->index_data_store && rec->index_store != NULL)
		return strdup(rec->index_store);
	return index_store_path(opts, project);
}

/* ------------------------------------------------------------------ */
/* The SDK statistics cache                                            */
/* ------------------------------------------------------------------ */

/*
 * -ivfsstatcache names a file in the SDKStatCaches.noindex directory, called
 * <sdk>-<build>-<64 hex digits>.sdkstatcache.  The hex part is a hash this
 * tool cannot compute, so the directory is searched for the one file whose
 * name begins with the SDK's own name, which is the part it can.
 *
 * The name is reported whole.  An internal SDK's cache was once reported with
 * only its first sixteen digits, which looked like a rule; it was a stale
 * cache, and the file that is on disk now is the one Xcode names.
 *
 * The directory sits directly under the derived data root, beside the
 * per-project directories rather than inside one, so it moves with a
 * -derivedDataPath and is looked for wherever the caller says that is.
 */
static char *statcache_path(const xcodebuild_opts *opts, const char *sdk_name)
{
	static const char ext[] = ".sdkstatcache";
	static const char sub[] = "/SDKStatCaches.noindex";
	char dir[PATH_MAX];
	char *root, *best = NULL;
	size_t best_len = 0;
	size_t slen, len;
	DIR *d;
	struct dirent *ent;

	if (sdk_name == NULL || *sdk_name == '\0')
		return NULL;
	if ((root = derived_data_root(opts)) == NULL)
		return NULL;
	len = strlen(root) + sizeof(sub);
	if (len > sizeof(dir)) {
		free(root);
		return NULL;
	}
	snprintf(dir, sizeof(dir), "%s%s", root, sub);
	free(root);
	if ((d = opendir(dir)) == NULL)
		return NULL;

	slen = strlen(sdk_name);
	while ((ent = readdir(d)) != NULL) {
		size_t nlen = strlen(ent->d_name);

		if (strncmp(ent->d_name, sdk_name, slen) != 0 ||
		    ent->d_name[slen] != '-')
			continue;
		if (nlen <= sizeof(ext) - 1 ||
		    strcmp(ent->d_name + nlen - (sizeof(ext) - 1), ext) != 0)
			continue;
		if (nlen > best_len) {	/* the longest match, i.e. the full one */
			free(best);
			best = strdup(ent->d_name);
			best_len = nlen;
		}
	}
	closedir(d);
	if (best == NULL)
		return NULL;

	{
		char full[PATH_MAX];

		snprintf(full, sizeof(full), "%s/%s", dir, best);
		free(best);
		return strdup(full);
	}
}

/* ------------------------------------------------------------------ */
/* One source file's record                                           */
/* ------------------------------------------------------------------ */

typedef struct {
	const xcodebuild_opts *opts;
	const settings_table *t;
	const char *source;
	char *index_store;	/* the arena, or NULL when this build indexes none */
	char *arch;
	char *dialect;		/* the LanguageDialect field */
	char *xarg;		/* what -x is given */
	char *sysroot;
	char *products;		/* BUILT_PRODUCTS_DIR */
	char *temp;		/* TARGET_TEMP_DIR */
	char *srcroot;
	char *object;		/* the -o path */
	char *unit_output;	/* the outputFilePath field */
	strvec defines;		/* the -D and -U words of OTHER_CFLAGS */
} index_file;

static void dialect_for(const char *path, char *dialect, size_t dlen,
    char *xarg, size_t xlen)
{
	static const struct {
		const char *ext;
		const char *dialect;
		const char *xarg;
	} table[] = {
		{ ".c",     "Xcode.SourceCodeLanguage.C",     "c" },
		{ ".m",     "Xcode.SourceCodeLanguage.ObjC",   "objective-c" },
		{ ".mm",    "Xcode.SourceCodeLanguage.ObjC++", "objective-c++" },
		{ ".cpp",   "Xcode.SourceCodeLanguage.C++",    "c++" },
		{ ".cc",    "Xcode.SourceCodeLanguage.C++",    "c++" },
		{ ".cxx",   "Xcode.SourceCodeLanguage.C++",    "c++" },
		{ ".c++",   "Xcode.SourceCodeLanguage.C++",    "c++" },
		{ ".swift", "Xcode.SourceCodeLanguage.Swift",  "swift" },
	};
	const char *ext = strrchr(path, '.');

	if (ext != NULL) {
		for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
			if (strcmp(ext, table[i].ext) == 0) {
				snprintf(dialect, dlen, "%s", table[i].dialect);
				snprintf(xarg, xlen, "%s", table[i].xarg);
				return;
			}
		}
	}
	snprintf(dialect, dlen, "Xcode.SourceCodeLanguage.C");
	snprintf(xarg, xlen, "c");
}

/* The one architecture the vector names.  ARCHS is a list whose first entry
 * is the one built.  CURRENT_ARCH is not consulted: this tool leaves it at
 * the placeholder Xcode itself reports for a target that is not being built,
 * and "undefined_arch" is not an architecture. */
static char *resolve_arch(const settings_table *t)
{
	char *archs = value_of(t, "ARCHS", "");
	char *first = first_word(archs);

	free(archs);
	if (*first == '\0' || strcmp(first, "undefined_arch") == 0) {
		free(first);
		return value_of(t, "NATIVE_ARCH", "");
	}
	return first;
}

static void index_file_open(index_file *f, const xcodebuild_opts *opts,
    const settings_table *t, const char *source, const char *index_store)
{
	char dbuf[128], xbuf[32], leaf[PATH_MAX];
	char *objroot;

	memset(f, 0, sizeof(*f));
	f->opts = opts;
	f->t = t;
	f->source = source;
	f->index_store = (index_store != NULL) ? strdup(index_store) : NULL;

	dialect_for(source, dbuf, sizeof(dbuf), xbuf, sizeof(xbuf));
	f->dialect = strdup(dbuf);
	f->xarg = strdup(xbuf);
	f->arch = resolve_arch(t);
	f->sysroot = value_of(t, "SDKROOT", "");
	f->products = value_of(t, "BUILT_PRODUCTS_DIR", "");
	f->temp = value_of(t, "TARGET_TEMP_DIR", "");
	f->srcroot = value_of(t, "SRCROOT", "");

	object_name(source, leaf, sizeof(leaf));
	{
		char obj[PATH_MAX * 2];

		snprintf(obj, sizeof(obj), "%s/Objects-normal/%s/%s", f->temp,
		    f->arch, leaf);
		f->object = strdup(obj);
	}

	/*
	 * An indexing build reports its object with the OBJROOT prefix gone and
	 * a leading slash left behind --
	 *
	 *	/Sources.build/Debug/hello.build/Objects-normal/arm64/main.o
	 *
	 * -- and that shortened form is also what -index-unit-output-path is
	 * given, so the field and the argument travel together.  A build that
	 * indexes nothing reports the whole path and passes no such argument,
	 * which is what the two fixtures differ on: the same project file with
	 * its scheme's configuration changed from Debug to Release stops
	 * indexing, and the path goes back to being absolute.
	 */
	f->unit_output = strdup(f->object);
	if (f->index_store != NULL) {
		objroot = value_of(t, "OBJROOT", "");
		if (*objroot != '\0' && strncmp(f->object, objroot,
		    strlen(objroot)) == 0) {
			free(f->unit_output);
			f->unit_output = strdup(f->object + strlen(objroot));
		}
		free(objroot);
	}
}

static void index_file_close(index_file *f)
{
	free(f->index_store);
	free(f->arch);
	free(f->dialect);
	free(f->xarg);
	free(f->sysroot);
	free(f->products);
	free(f->temp);
	free(f->srcroot);
	free(f->object);
	free(f->unit_output);
	sv_free(&f->defines);
	memset(f, 0, sizeof(*f));
}

static int has_arena(const index_file *f)
{
	return f->index_store != NULL;
}

/* The vector, in the order Apple writes it. */
static void index_file_arguments(index_file *f, strvec *out)
{
	static const char *const optflags[] = {
		"-O0", "-O1", "-O2", "-O3", "-Os", "-Og",
	};
	const settings_table *t = f->t;
	char *opt = value_of(t, "GCC_OPTIMIZATION_LEVEL", "");
	char *dbg = value_of(t, "DEBUGGING_SYMBOLS", "");
	char *pascal = value_of(t, "PASCAL_STRINGS", "");
	char *std = value_of(t, "GCC_C_LANGUAGE_STANDARD", "");
	/*
	 * Xcode's default for this is YES for a program and unset for a
	 * library, and Apple's tool reports the key only where the default
	 * stands it up: a dylib target here compiles without
	 * -fvisibility=hidden, a tool target with, neither having said anything
	 * in the project.  So the default follows the kind of product.
	 */
	char *private_extern = value_of(t, "GCC_SYMBOLS_PRIVATE_EXTERN",
	    is_library_product(t) ? "NO" : "YES");
	char *deploy = value_of(t, "MACOSX_DEPLOYMENT_TARGET", "");
	char *product = value_of(t, "PRODUCT_NAME", "");
	char *sdk_name = value_of(t, "SDK_NAME", "");
	char *statcache;
	strvec hpaths = {0}, seen = {0};

	sv_push(out, "-x");
	sv_push(out, f->xarg);

	if ((statcache = statcache_path(f->opts, sdk_name)) != NULL) {
		sv_push(out, "-ivfsstatcache");
		sv_push(out, statcache);
		free(statcache);
	}

	sv_push(out, "-target");
	sv_pushf(out, "%s-apple-macos%s", f->arch,
	    (*deploy != '\0') ? deploy : "13.0");

	for (size_t i = 0; i < sizeof(kDiagnostics) / sizeof(kDiagnostics[0]); i++)
		sv_push(out, kDiagnostics[i]);

	sv_push(out, "-isysroot");
	sv_push(out, f->sysroot);

	for (size_t i = 0; i < sizeof(kAfterSysroot) / sizeof(kAfterSysroot[0]); i++)
		sv_push(out, kAfterSysroot[i]);

	if (has_arena(f)) {
		sv_push(out, "-index-store-path");
		sv_push(out, f->index_store);
	}

	/*
	 * OTHER_CFLAGS in place, but its -D and -U words are held back: they
	 * go out again after the include paths, which is where both fixtures
	 * put them.
	 */
	{
		char *other = value_of(t, "OTHER_CFLAGS", "");
		char *word, *save = NULL;

		for (word = strtok_r(other, " \t", &save); word != NULL;
		    word = strtok_r(NULL, " \t", &save)) {
			if (word[0] == '-' && (word[1] == 'D' || word[1] == 'U'))
				sv_push(&f->defines, word);
			else
				sv_push(out, word);
		}
		free(other);
	}

	if (*std != '\0')
		sv_pushf(out, "-std=%s", std);
	if (is_yes(pascal))
		sv_push(out, "-fpascal-strings");
	for (size_t i = 0; i < sizeof(optflags) / sizeof(optflags[0]); i++) {
		if (strcmp(opt, optflags[i] + 2) == 0) {
			sv_push(out, optflags[i]);
			break;
		}
	}
	if (is_yes(dbg))
		sv_push(out, "-g");
	if (is_yes(private_extern))
		sv_push(out, "-fvisibility=hidden");

	/*
	 * The four header maps, in the order the compiler wants them and with
	 * the quoting each is searched with: the generated map and the project
	 * map with -iquote, the two target maps with -I.  The prefix is the
	 * product name, which for a target that names none is empty -- hence
	 * "-generated-files.hmap" and not "generated-files.hmap".
	 */
	sv_push(out, "-iquote");
	sv_pushf(out, "%s/%s-generated-files.hmap", f->temp, product);
	sv_pushf(out, "-I%s/%s-own-target-headers.hmap", f->temp, product);
	sv_pushf(out, "-I%s/%s-all-target-headers.hmap", f->temp, product);
	sv_push(out, "-iquote");
	sv_pushf(out, "%s/%s-project-headers.hmap", f->temp, product);

	/* The products directory's include folder is on the path whether or
	 * not the project mentions it, and it comes before HEADER_SEARCH_PATHS
	 * rather than after. */
	sv_pushf(out, "-I%s/include", f->products);

	{
		char *hsp = value_of(t, "HEADER_SEARCH_PATHS", "");

		push_words(&hpaths, hsp);
		free(hsp);
	}
	for (size_t i = 0; i < hpaths.n; i++) {
		char flag[PATH_MAX + 3];

		snprintf(flag, sizeof(flag), "-I%s", hpaths.v[i]);
		if (!sv_has(out, flag) && !sv_has(&seen, flag)) {
			sv_push(out, flag);
			sv_push(&seen, flag);
		}
	}
	sv_free(&hpaths);
	sv_free(&seen);

	sv_pushf(out, "-I%s/DerivedSources-normal/%s", f->temp, f->arch);
	sv_pushf(out, "-I%s/DerivedSources/%s", f->temp, f->arch);
	sv_pushf(out, "-I%s/DerivedSources", f->temp);
	sv_pushf(out, "-F%s", f->products);

	for (size_t i = 0; i < f->defines.n; i++)
		sv_push(out, f->defines.v[i]);
	sv_free(&f->defines);

	sv_push(out, "-fsyntax-only");
	sv_push(out, f->source);
	sv_push(out, "-o");
	sv_push(out, f->object);

	if (has_arena(f) && strcmp(f->unit_output, f->object) != 0) {
		sv_push(out, "-index-unit-output-path");
		sv_push(out, f->unit_output);
	}

	sv_pushf(out, "-working-directory=%s", f->srcroot);
	for (size_t i = 0; i < sizeof(kTail) / sizeof(kTail[0]); i++)
		sv_push(out, kTail[i]);

	free(opt);
	free(dbg);
	free(pascal);
	free(std);
	free(private_extern);
	free(deploy);
	free(product);
	free(sdk_name);
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

/*
 * The layout is CoreFoundation's, not this repository's: two spaces a level,
 * a space either side of the colon, and an empty object written as an open
 * brace, a blank line and the close.  The -showBuildSettings printer cannot
 * be reused for it -- that one separates with ":" and leaves an empty object
 * as "{}" -- so the two forms are printed by two printers.
 */
static void print_indent(FILE *fp, int indent)
{
	for (int i = 0; i < indent; i++)
		fputc(' ', fp);
}

static void print_json_string(FILE *fp, const char *s)
{
	fputc('"', fp);
	for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
		switch (*p) {
		case '"':  fputs("\\\"", fp); break;
		case '\\': fputs("\\\\", fp); break;
		case '\n': fputs("\\n", fp); break;
		case '\r': fputs("\\r", fp); break;
		case '\t': fputs("\\t", fp); break;
		default:
			if (*p < 0x20)
				fprintf(fp, "\\u%04x", *p);
			else
				fputc(*p, fp);
			break;
		}
	}
	fputc('"', fp);
}

static void print_string_array(FILE *fp, char *const *v, size_t n, int indent)
{
	if (n == 0) {
		fputs("[\n\n", fp);
		print_indent(fp, indent);
		fputc(']', fp);
		return;
	}
	fputs("[\n", fp);
	for (size_t i = 0; i < n; i++) {
		print_indent(fp, indent + 2);
		print_json_string(fp, v[i]);
		fputs((i + 1 < n) ? ",\n" : "\n", fp);
	}
	print_indent(fp, indent);
	fputc(']', fp);
}

/* One source file's six fields, at `indent`, where the fields sit two
 * further in. */
static void print_record(FILE *fp, index_file *f, int indent)
{
	char asset[PATH_MAX * 2];

	snprintf(asset, sizeof(asset), "%s/DerivedSources/GeneratedAssetSymbols-Index.plist",
	    f->temp);

	fputs("{\n", fp);

	print_indent(fp, indent + 2);
	fputs("\"assetSymbolIndexPath\" : ", fp);
	print_json_string(fp, asset);
	fputs(",\n", fp);

	print_indent(fp, indent + 2);
	fputs("\"clangASTBuiltProductsDir\" : ", fp);
	print_json_string(fp, f->products);
	fputs(",\n", fp);

	{
		strvec args = {0};

		index_file_arguments(f, &args);
		print_indent(fp, indent + 2);
		fputs("\"clangASTCommandArguments\" : ", fp);
		print_string_array(fp, args.v, args.n, indent + 2);
		sv_free(&args);
	}
	fputs(",\n", fp);

	print_indent(fp, indent + 2);
	fputs("\"LanguageDialect\" : ", fp);
	print_json_string(fp, f->dialect);
	fputs(",\n", fp);

	print_indent(fp, indent + 2);
	fputs("\"outputFilePath\" : ", fp);
	print_json_string(fp, f->unit_output);
	fputs(",\n", fp);

	print_indent(fp, indent + 2);
	fputs("\"toolchains\" : ", fp);
	print_string_array(fp, (char *const *)kToolchains,
	    sizeof(kToolchains) / sizeof(kToolchains[0]), indent + 2);
	fputc('\n', fp);

	print_indent(fp, indent);
	fputc('}', fp);
}

/* Paths in byte order, for qsort. */
static int compare_path(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The object of source path to record.  `outer` is the indent of the line the
 * target's own name is on, and the sources are one level in from there, so a
 * console block starts its sources at two and a -json one at four.  A target
 * that compiles no file that exists is a blank line and not "{}": its closing
 * brace stays at `outer`, level with the name, where Apple puts it.
 * `name` is the resolved target, not the -target string: asked for nothing in
 * particular the project still has one target, and it is that one whose
 * sources are listed. */
static void print_target(FILE *fp, const char *name,
    const xcodebuild_opts *opts, settings_table *t, const char *project,
    char *index_store, int outer)
{
	char **sources = NULL;
	int n = (project != NULL) ?
	    xcindex_target_sources(project, name, &sources) : 0;

	if (n <= 0) {
		fputs("{\n\n", fp);
		print_indent(fp, outer);
		fputc('}', fp);
		goto done;
	}

	/*
	 * In path order, not in the order the project lists the files.  A
	 * project's phases name its sources in whatever order they were added,
	 * and Apple's tool prints them sorted, so a phase holding one file from
	 * each of two directories comes out with those directories' files
	 * interleaved rather than grouped by phase.
	 */
	qsort(sources, (size_t)n, sizeof(sources[0]), compare_path);

	fputs("{\n", fp);
	for (int i = 0; i < n; i++) {
		index_file f;

		if (i > 0)
			fputs(",\n", fp);
		print_indent(fp, outer + 2);
		print_json_string(fp, sources[i]);
		fputs(" : ", fp);
		index_file_open(&f, opts, t, sources[i], index_store);
		print_record(fp, &f, outer + 2);
		index_file_close(&f);
	}
	fputc('\n', fp);
	print_indent(fp, outer);
	fputc('}', fp);

done:
	for (int i = 0; i < n; i++)
		free(sources[i]);
	free(sources);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void emit_index_target(const xcodebuild_opts *opts, const char *devpath,
    const char *project, const char *name, const index_record *rec,
    xcodebuild_opts *effective, int indent, int *first)
{
	settings_table *t = xbuild_settings_for_target(effective, devpath, name,
	    project);
	const char *tname;
	char *store;

	if (t == NULL)
		return;

	/* The name is the resolved one, as it is for -showBuildSettings:
	 * asked for nothing in particular, Apple's tool names the target it
	 * settled on, and a walk that reached a target by name still reports
	 * what the project calls that target. */
	tname = settings_get_or(t, "TARGET_NAME", (name != NULL) ? name : "");

	/* Every source of one target shares the arena, so it is settled once
	 * here and each file's record is handed its own copy. */
	store = index_store_for(opts, t, project, effective->configuration, rec);

	if (opts->json) {
		if (!*first)
			fputs(",\n", stdout);
		*first = 0;
		print_indent(stdout, indent);
		print_json_string(stdout, tname);
		fputs(" : ", stdout);
		print_target(stdout, tname, opts, t, project, store, indent);
	} else {
		printf("Build settings for target %s:\n", tname);
		print_target(stdout, tname, opts, t, project, store, indent);
		fputc('\n', stdout);
	}
	free(store);
	settings_destroy(t);
}

/* A target as -json walks it, paired so the sort carries both. */
struct named_target {
	char *name;
	char *guid;
};

static int compare_named_target(const void *a, const void *b)
{
	const struct named_target *x = a, *y = b;

	return strcmp(x->name, y->name);
}

int xcodebuild_emit_index_settings(const xcodebuild_opts *opts,
    const char *devpath)
{
	xcodebuild_opts effective = *opts;
	index_record rec;
	char **names = NULL, **guids = NULL;
	char *project;
	char scheme[256];
	int n, first = 1;
	int indent = opts->json ? 2 : 0;

	/*
	 * "-alltargets and also specify individual targets" is Apple wording
	 * for one refusal, as it is for -showBuildSettings: the two ask for
	 * two different sets.
	 */
	if (opts->all_targets && opts->target != NULL) {
		fprintf(stderr, "xcodebuild: error: You cannot specify "
		    "-alltargets and also specify individual targets.\n");
		return 64;
	}

	project = xbuild_detect_project(opts);
	/*
	 * Absolute, because everything downstream compares or joins it:
	 * DerivedData's info.plist records the workspace path in full, and the
	 * source paths a project contributes are written relative to it.  The
	 * path need not exist for this -- xc_abspath resolves "." and ".."
	 * textually rather than touching the filesystem.
	 */
	if (project != NULL) {
		char absolute[PATH_MAX];

		if (xc_abspath(project, absolute, sizeof(absolute)) != NULL) {
			free(project);
			project = strdup(absolute);
		}
	}
	index_record_load(opts, project, &rec);

	/*
	 * An index build answers from the project, not from the command line:
	 * Apple ignores -configuration, -sdk and KEY=VALUE here, so the copy is
	 * stripped of them.
	 *
	 * The configuration comes from the project's shared scheme, which
	 * outranks both the record of an earlier index build and the project's
	 * own defaultConfigurationName; a project with no shared scheme falls
	 * back to the record, and one with neither falls back to its default.
	 * The scheme has to be consulted first, or the answer depends on
	 * whether anything has ever indexed this project at this path: the
	 * record is kept in DerivedData under a key derived from the project's
	 * path, so the same project at a new path would answer differently.
	 */
	if (project_scheme_configuration(project, scheme, sizeof(scheme)) != NULL)
		effective.configuration = scheme;
	else
		effective.configuration = rec.configuration;
	effective.sdk = NULL;
	effective.overrides = NULL;
	effective.n_overrides = 0;

	/*
	 * Every -target is answered separately, in the order they were given,
	 * and all of them are checked before anything at all is printed.
	 * Asked for one that exists and one that does not, Apple refuses the
	 * request and prints nothing -- not even, in -json, the brace that
	 * would have opened the object -- so a record emitted for the first
	 * before the refusal would be a difference in what the index service
	 * was handed.
	 *
	 * The project is named as the command line named it: an argument that
	 * came in relative, or with a trailing slash, or in full, is reported
	 * the same way, since that is the only form of the name the caller can
	 * recognise.  Status 65 is Apple's for a request it cannot answer.
	 */
	if (project != NULL && opts->n_targets > 0) {
		char **known = NULL, **known_guids = NULL;
		int n_known = xcindex_target_list(project, &known,
		    &known_guids);
		int unknown = 0;

		for (int i = 0; i < opts->n_targets && !unknown; i++) {
			int found = 0;

			for (int j = 0; j < n_known; j++) {
				if (known[j] != NULL &&
				    strcmp(known[j], opts->targets[i]) == 0) {
					found = 1;
					break;
				}
			}
			if (!found) {
				fprintf(stderr, "xcodebuild: error: The project "
				    "'%s' does not contain a target named '%s'.\n",
				    (opts->project != NULL) ?
				    opts->project : project,
				    opts->targets[i]);
				unknown = 1;
			}
		}
		for (int j = 0; j < n_known; j++) {
			free(known[j]);
			free(known_guids[j]);
		}
		free(known);
		free(known_guids);
		if (unknown)
			return 65;
	}

	if (opts->json)
		fputs("{\n", stdout);

	if (opts->n_targets > 0) {
		for (int i = 0; i < opts->n_targets; i++) {
			/*
			 * A repeated -target is answered once per occurrence in
			 * the console form, since each is a block of its own,
			 * and once in -json, since a repeated key is not a
			 * thing.  The console walk's closing repeat belongs to
			 * the walk and not here.
			 */
			int dup = 0;

			if (opts->json) {
				for (int j = 0; j < i; j++) {
					if (strcmp(opts->targets[j],
					    opts->targets[i]) == 0)
						dup = 1;
				}
			}
			if (dup)
				continue;
			emit_index_target(opts, devpath, project,
			    opts->targets[i], &rec, &effective, indent, &first);
		}
		n = 0;
	} else if (!opts->all_targets) {
		emit_index_target(opts, devpath, project, NULL, &rec,
		    &effective, indent, &first);
		n = 0;
	} else {
		n = (project != NULL) ?
		    xcindex_target_list(project, &names, &guids) : 0;

		/*
		 * The console form prints the first target a second time at
		 * the end of the walk, as -showBuildSettings does, and keeps
		 * the order the project lists them in.  -json has no use for
		 * a repeated key, so there each name is printed once, and in
		 * order by name: the goldens come out that way, loader before
		 * xcodebuild, hello before world.
		 */
		if (opts->json && n > 0) {
			struct named_target *order =
			    malloc((size_t)n * sizeof(*order));

			for (int i = 0; i < n; i++) {
				order[i].name = names[i];
				order[i].guid = guids[i];
			}
			qsort(order, (size_t)n, sizeof(order[0]),
			    compare_named_target);
			for (int i = 0; i < n; i++) {
				names[i] = order[i].name;
				guids[i] = order[i].guid;
			}
			free(order);
		}

		for (int i = 0; i < n; i++) {
			if (opts->json) {
				int dup = 0;

				for (int j = 0; j < i; j++) {
					if (names[j] != NULL && names[i] != NULL &&
					    strcmp(names[j], names[i]) == 0)
						dup = 1;
				}
				if (dup)
					continue;
			}
			emit_index_target(opts, devpath, project, names[i], &rec,
			    &effective, indent, &first);
		}
		if (!opts->json && n > 0)
			emit_index_target(opts, devpath, project, names[0], &rec,
			    &effective, indent, &first);
	}

	for (int i = 0; i < n; i++) {
		free(names[i]);
		free(guids[i]);
	}
	free(names);
	free(guids);
	free(project);
	index_record_free(&rec);

	if (opts->json) {
		fputs("\n}\n", stdout);
	}
	return 0;
}
