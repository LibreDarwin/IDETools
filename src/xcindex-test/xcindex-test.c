/*
 * xcindex-test.c -- the third IDETools product: Apple's index test driver.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Open-source reimplementation of Apple's xcindex-test, the utility that
 * exercises the build system APIs the index service interacts with.  Apple's
 * binary is at <Xcode>/Contents/Developer/usr/bin/xcindex-test.
 *
 * The tool is a thin driver.  It owns a command line whose options live
 * before the first "--" and whose console commands live after it, one per
 * "--" separated group; after the command line runs, it reads more console
 * commands from stdin, one per line, until EOF or "quit".  Every command
 * either prints a deterministic report derived from the project (this file
 * and project.c) or is handed to a build engine that this driver only
 * names for now.
 *
 * The framing is Apple's, down to the whitespace: every command prints a
 * "Finished loading workspace in N seconds" line once the project is open,
 * its own report, and a "command succeeded in N seconds" line; errors go
 * to stderr as "error: ..." followed by a one-line hint whose text depends
 * on whether the command came from the command line or the REPL.  The
 * deterministic half of that is reproduced here; the engine half -- build
 * descriptions, build settings, indexing -- is a later step and is stubbed
 * behind the same entry points.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#include "xcindex-help.h"
#include "project.h"
#include "xcpath.h"

/* ------------------------------------------------------------------ */
/* Small growable string vector                                       */
/* ------------------------------------------------------------------ */

struct strvec {
	char **items;
	int count;
	int capacity;
};

static void
strvec_push(struct strvec *v, const char *s)
{
	char **grown;

	if (v->count == v->capacity) {
		int cap = v->capacity ? v->capacity * 2 : 8;
		grown = realloc(v->items, (size_t)cap * sizeof(*grown));
		if (grown == NULL)
			return;
		v->items = grown;
		v->capacity = cap;
	}
	v->items[v->count] = strdup(s);
	v->count++;
}

static void
strvec_free(struct strvec *v)
{
	for (int i = 0; i < v->count; i++)
		free(v->items[i]);
	free(v->items);
	v->items = NULL;
	v->count = v->capacity = 0;
}

/* Is @arg s already in the vector?  Used to fold a scheme's repeated
 * BuildableReferences down to one entry per target. */
static int
strvec_contains(const struct strvec *v, const char *s)
{
	for (int i = 0; i < v->count; i++)
		if (strcmp(v->items[i], s) == 0)
			return 1;
	return 0;
}

/*
 * Target names sort by strcmp.  The oracle does not case-fold, so an
 * upper-case name precedes a lower-case one that would otherwise sort
 * earlier ("Bee" before "apple").
 */
static int
strvec_name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ------------------------------------------------------------------ */
/* Console option state                                               */
/* ------------------------------------------------------------------ */

/*
 * One console command's parsed options.  Apple models these as a struct
 * with a set of optional strings; the driver only needs the ones that
 * affect the deterministic commands, plus a few that gate engine
 * commands.  A value option that is given no value (the next token is
 * absent, or is itself a "-..." option) records the empty string, which is
 * what Apple resolves such an option to.
 */
struct console_opts {
	const char *action;

	const char *destination;
	const char *scheme;
	struct strvec targets;             /* -target, in order, may repeat */
	const char *targets_of_scheme;
	const char *targets_not_of_scheme;
	int all_targets;

	const char *ignore_list_path;
	int skip_first_n;                   /* < 0 when absent */
	const char *build_description;
	const char *file;
	int output_path_only;
	int use_build_manager;
	int prepare_dependencies;
	int high_priority;
	const char *configuration;

	const char *qos;
	int jobs;                          /* < 0 when absent */

	int verbose;
	int quiet;

	/* Set when an option token was not recognized.  Apple reports the
	 * first unknown argument (in practice the hash order of the ones
	 * collected, which is why a test uses a single one). */
	const char *unknown_arg;
};

static void
console_opts_init(struct console_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->skip_first_n = -1;
	o->jobs = -1;
}

/* The recognised string-valued options, in the order they are declared. */
static const char *const kValueOptions[] = {
	"-destination", "-scheme", "-target", "-targets-of-scheme",
	"-targets-not-of-scheme", "-ignore-list-path", "-skip-first-n",
	"-build-description", "-file", "-configuration", "-qos", "-j",
	NULL
};

/* The recognised flag options (those taking no value). */
static const char *const kFlagOptions[] = {
	"-all-targets", "-output-path-only", "-use-build-manager",
	"-prepare-dependencies", "-high-priority", "-v", "-quiet",
	NULL
};

static int
in_list(const char *const *list, const char *arg)
{
	for (int i = 0; list[i] != NULL; i++)
		if (strcmp(list[i], arg) == 0)
			return 1;
	return 0;
}

/*
 * A value option takes the next token as its value only when that token
 * exists and is not itself an option.  Apple parses the command line
 * left to right: a value option immediately followed by another "-..."
 * token leaves the option empty and lets the following token be examined
 * as an option itself.  That is what makes "-target -foo" report "-foo" as
 * an unknown argument rather than swallow it as a target name.
 */
static const char *
option_value(int argc, char **argv, int *i)
{
	if (*i + 1 < argc && argv[*i + 1][0] != '-')
		return argv[++(*i)];
	return "";
}

/* ------------------------------------------------------------------ */
/* Validation, in Apple's fixed precedence                             */
/* ------------------------------------------------------------------ */

/*
 * Apple validates console options in a fixed order that is independent of
 * where the options appear on the line.  The order, established by
 * probing, is:
 *
 *   1. -qos is a closed set, so an unknown name fails first of all.
 *   2. -j and -skip-first-n must parse as a non-negative integer; a
 *      missing or malformed value is an InvalidArgumentValueError.
 *   3. an unrecognised option is reported next.
 *   4. -scheme must resolve.
 *   5. each -target must name a target, in the order given, and the first
 *      failure is the one reported.
 *
 * Because the checks are separated from parsing, "-qos bogus" outranks a
 * bad -j and both outrank an unknown option, regardless of order.
 */

static int
qos_valid(const char *name)
{
	return strcmp(name, "background") == 0 ||
	    strcmp(name, "utility") == 0 ||
	    strcmp(name, "default") == 0 ||
	    strcmp(name, "userInitiated") == 0;
}

/* A non-negative decimal integer, nothing else: -1 and "abc" both fail. */
static int
parse_count(const char *s, int *out)
{
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return 0;
	v = strtol(s, &end, 10);
	if (*end != '\0' || v < 0 || v > 0x7fffffff)
		return 0;
	*out = (int)v;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

struct driver {
	const char *project;                /* absolute project path */
	const char *query;                  /* the bundle, or NULL if not one */
	int repl;                           /* 1 in REPL mode: hints differ */
	int load_reported;                  /* the load line is printed once */
};

/*
 * Apple opens an .xcodeproj bundle and nothing else.  Given the pbxproj
 * file itself, or a plain directory that happens to hold one, it loads
 * without complaint and reports a project with no targets and no schemes,
 * so every command succeeds with an empty report.  A NULL query is how the
 * rest of the driver learns that.
 */
static int
project_is_bundle(const char *path)
{
	static const char ext[] = ".xcodeproj";
	size_t n = strlen(path);

	return n > sizeof(ext) - 1 &&
	    strcmp(path + n - (sizeof(ext) - 1), ext) == 0;
}

/*
 * The one-line hint Apple appends to most errors.  Its text depends on
 * where the failing command came from: the command line says "-help",
 * the REPL says "help".  The errors that carry no hint at all -- a
 * scheme that does not resolve, a missing run destination, a missing
 * active scheme, a missing build description, the file-does-not-exist
 * error -- deliberately fall through here with suffix == 0.
 */
static void
print_error(const struct driver *d, const char *message, int suffix)
{
	fprintf(stderr, "error: %s\n", message);
	if (suffix)
		fprintf(stderr, d->repl
		    ? "Write 'help' for information about available actions and options\n"
		    : "See -help for information about available commands and options\n");
}

/*
 * Seconds, as Apple prints them, for the two framing lines.  The precision
 * matters: a near-instant operation still prints a non-zero mantissa, so a
 * plain "%g" -- which would render a sub-microsecond elapsed as a flat
 * "0" -- does not match.  Apple prints the full double expansion, so the
 * value is formatted the same way here.
 */
static void
print_seconds(const char *prefix, double elapsed)
{
	/* A measurement that comes back exactly zero is an artifact of two
	 * back-to-back clock reads, not a real instant; Apple always prints a
	 * non-zero mantissa, so a flat "0" is floored to keep the shape of
	 * the line stable.  The comparison tests normalise the value away, so
	 * only the presence of a fractional part matters here. */
	if (elapsed <= 0.0)
		elapsed = 1e-6;
	printf("%s%.17g seconds\n", prefix, elapsed);
}

/*
 * Apple opens the project before it reads any command, not on first use:
 * a run with no input at all still prints the load line, and it appears
 * before the first command's own output.  "Skipping N targets" is the one
 * thing printed ahead of it -- that line is decided while the command's
 * arguments are parsed, which happens before the open is reported.
 */
static void
ensure_loaded(struct driver *d)
{
	struct timespec t0, t1;
	double elapsed;

	if (d->load_reported)
		return;
	d->load_reported = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	/* The project's contents are read on demand by each command; the
	 * load itself is the directory scan, which is immediate here. */
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed = (double)(t1.tv_sec - t0.tv_sec) +
	    (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	print_seconds("Finished loading workspace in ", elapsed);
}

/* ------------------------------------------------------------------ */
/* Target selection                                                    */
/* ------------------------------------------------------------------ */

/*
 * Apple composes the set of targets a command acts on as an ordered list
 * with duplicates kept, built by concatenating, in this order:
 *
 *   - every -target, in the order given;
 *   - the targets of every -targets-of-scheme;
 *   - the targets of every -targets-not-of-scheme, that is, the project's
 *     targets minus the named scheme's.
 *
 * The concatenation order is fixed, not the order the options appear in:
 * "-targets-of-scheme S -target T" still puts T first.  If the result is
 * empty -- no selector at all, or a -targets-not-of-scheme that removes
 * everything -- the selection falls back to every target.
 *
 * -all-targets overrides the whole thing: it selects every target and
 * suppresses validation of -target, -targets-of-scheme and
 * -targets-not-of-scheme entirely, so a bogus name beside it is not an
 * error.  It does not suppress -scheme, which is validated on its own.
 */

struct selection {
	struct strvec items;                /* target names, in order */
	int explicit_select;                /* a selector was given */
};

/* Is @arg a target of the project? */
static int
target_exists(const char *project, const char *name)
{
	char **names = NULL;
	int n = xcindex_target_list(project, &names, NULL);
	int found = 0;

	for (int i = 0; i < n; i++) {
		if (strcmp(names[i], name) == 0) {
			found = 1;
			break;
		}
	}
	for (int i = 0; i < n; i++)
		free(names[i]);
	free(names);
	return found;
}

/*
 * Every target of the project.  The order is the name order the oracle
 * prints, not the order the pbxproj lists its targets in: a project whose
 * targets array reads zzz then aaa is reported as aaa then zzz.  strcmp
 * rather than a case-folding compare, so "Bee" precedes "apple".
 */
static void
all_targets(const char *project, struct strvec *out)
{
	char **names = NULL;
	int n = xcindex_target_list(project, &names, NULL);

	for (int i = 0; i < n; i++)
		strvec_push(out, names[i]);
	for (int i = 0; i < n; i++)
		free(names[i]);
	free(names);

	if (out->count > 1)
		qsort(out->items, (size_t)out->count, sizeof(*out->items),
		    strvec_name_cmp);
}

/*
 * Build the ordered selection.  Returns 0 on success, or -1 after
 * reporting the first validation failure (in the precedence order fixed
 * above).  *fatal_target, when set, names the target that failed.
 */
static int
build_selection(const struct driver *d, const struct console_opts *o,
    struct selection *sel, const char **bad_scheme, const char **bad_target)
{
	/*
	 * The whole vector is cleared, not just the count: the caller hands
	 * over an uninitialised struct, and a stale capacity would make the
	 * first push reallocate a pointer that was never one.
	 */
	sel->items.items = NULL;
	sel->items.count = 0;
	sel->items.capacity = 0;
	sel->explicit_select = 0;

	if (o->all_targets) {
		all_targets(d->query, &sel->items);
		sel->explicit_select = 1;
		return 0;
	}

	/* -target, in the order given, each one validated. */
	for (int i = 0; i < o->targets.count; i++) {
		const char *name = o->targets.items[i];
		if (!target_exists(d->query, name)) {
			*bad_target = name;
			return -1;
		}
		sel->explicit_select = 1;
		strvec_push(&sel->items, name);
	}

	/*
	 * -targets-of-scheme, resolved to the targets its scheme selects.  A
	 * scheme names a target in more than one place -- a test action and a
	 * build action both carry a BuildableReference -- so the expansion is
	 * folded to one entry per target and then sorted by name, which is
	 * the order the oracle reports it in rather than the order the scheme
	 * file happens to list its references.
	 */
	if (o->targets_of_scheme != NULL) {
		char **names = NULL;
		int from_file = 0;

		if (!xcindex_scheme_resolve(d->query, o->targets_of_scheme,
		    &from_file)) {
			*bad_scheme = o->targets_of_scheme;
			return -1;
		}
		sel->explicit_select = 1;
		int n = xcindex_scheme_targets(d->query, o->targets_of_scheme,
		    &names);
		for (int i = 0; i < n; i++) {
			if (!strvec_contains(&sel->items, names[i]))
				strvec_push(&sel->items, names[i]);
			free(names[i]);
		}
		free(names);
		if (sel->items.count > 1)
			qsort(sel->items.items, (size_t)sel->items.count,
			    sizeof(*sel->items.items), strvec_name_cmp);
	}

	/*
	 * -targets-not-of-scheme: every target of the project that the named
	 * scheme does *not* select.  That is a set difference against the
	 * project's own targets walked in order, not a walk of the scheme's
	 * list, so a target the scheme never mentions is included and one it
	 * mentions is left out however many times it mentions it.
	 */
	if (o->targets_not_of_scheme != NULL) {
		char **in_scheme = NULL;
		int from_file = 0;

		if (!xcindex_scheme_resolve(d->query,
		    o->targets_not_of_scheme, &from_file)) {
			*bad_scheme = o->targets_not_of_scheme;
			return -1;
		}
		sel->explicit_select = 1;

		struct strvec project_targets = { 0 };
		all_targets(d->query, &project_targets);
		int sn = xcindex_scheme_targets(d->query,
		    o->targets_not_of_scheme, &in_scheme);

		for (int i = 0; i < project_targets.count; i++) {
			const char *name = project_targets.items[i];
			int excluded = 0;
			for (int k = 0; k < sn; k++) {
				if (strcmp(in_scheme[k], name) == 0) {
					excluded = 1;
					break;
				}
			}
			if (!excluded)
				strvec_push(&sel->items, name);
		}

		strvec_free(&project_targets);
		for (int i = 0; i < sn; i++)
			free(in_scheme[i]);
		free(in_scheme);
	}

	/* Empty (no selector, or a complement that removed everything) falls
	 * back to every target. */
	if (sel->items.count == 0)
		all_targets(d->query, &sel->items);

	return 0;
}

/* ------------------------------------------------------------------ */
/* The deterministic commands                                         */
/* ------------------------------------------------------------------ */

/*
 * list-schemes: every scheme file of the project plus every target-derived
 * scheme that is not suppressed, sorted case-sensitively.  project.c
 * assembles the list; the driver only prints it.
 */
static int
cmd_list_schemes(struct driver *d)
{
	char **names = NULL;
	int n;

	n = xcindex_scheme_list(d->query, &names);
	for (int i = 0; i < n; i++) {
		printf("%s\n", names[i]);
		free(names[i]);
	}
	free(names);
	return 0;
}

/*
 * The index id Apple composes for a target is the project bundle's own
 * path, then the target's name and its object id.  When -project names
 * the .pbxproj directly the bundle is its parent, so a .pbxproj path is
 * cut down first; that is the same normalisation project.c applies when
 * it matches a scheme's container.
 */
static void
target_id(const char *project, const char *name, const char *guid,
    char *buf, size_t len)
{
	char bundle[PATH_MAX];
	char dir[PATH_MAX];
	const char *base = project;
	const char *slash;

	if (strstr(project, ".pbxproj") != NULL &&
	    xc_dirname(project, dir, sizeof(dir)) != NULL)
		base = dir;

	/* Keep the bundle path, drop any trailing component that is not
	 * part of the bundle name -- for ".xcodeproj" the last component
	 * *is* the bundle, so nothing is dropped. */
	snprintf(bundle, sizeof(bundle), "%s", base);
	(void)slash;

	snprintf(buf, len, "%s/%s-%s", bundle, name, guid);
}

/* The absolute path of one of a target's on-disk sources. */
static int
target_source(const char *project, const char *target, char *buf, size_t len)
{
	char **paths = NULL;
	int n = xcindex_target_sources(project, target, &paths);
	int have = 0;

	if (n > 0 && paths[0] != NULL) {
		snprintf(buf, len, "%s", paths[0]);
		have = 1;
	}
	for (int i = 0; i < n; i++)
		free(paths[i]);
	free(paths);
	return have;
}

/*
 * list-indexables: one block per selected target, in selection order, with
 * the target's index id, its blueprint flag, and the source file it
 * compiles.  The id and the source path are absolute, which is why the
 * project is resolved to an absolute path before anything is printed.
 */
static int
cmd_list_indexables(struct driver *d, const struct console_opts *o,
    struct selection *sel)
{

	/* -skip-first-n drops the first N of the selection, but only when a
	 * selector narrowed it; on the default (every target) selection it
	 * is not applied and no line is printed. */
	int start = 0;
	if (o->skip_first_n > 0 && sel->explicit_select) {
		start = o->skip_first_n;
		if (start > sel->items.count)
			start = sel->items.count;
	}

	for (int i = start; i < sel->items.count; i++) {
		const char *name = sel->items.items[i];
		char idbuf[2048], srcbuf[2048];

		/* Resolve the target's object id for the index id. */
		char **names = NULL, **guids = NULL;
		int n = xcindex_target_list(d->query, &names, &guids);
		const char *guid = "";
		for (int k = 0; k < n; k++) {
			if (strcmp(names[k], name) == 0) {
				guid = guids[k];
				break;
			}
		}

		printf("--- %s\n", name);
		target_id(d->query, name, guid, idbuf, sizeof(idbuf));
		printf("id: %s\n", idbuf);
		printf("blueprint: true\n");
		if (target_source(d->query, name, srcbuf, sizeof(srcbuf)))
			printf("    %s\n", srcbuf);
		printf("========\n");

		for (int k = 0; k < n; k++) {
			free(names[k]);
			free(guids[k]);
		}
		free(names);
		free(guids);
	}
	return 0;
}

/*
 * print-stats: the counts over the same selection list-indexables walks,
 * so duplicates are counted and the default selection reports every
 * target.  "blueprints" equals the target count because every target here
 * is a blueprint; "source files" is the number of on-disk sources across
 * the selection.
 */
static int
cmd_print_stats(struct driver *d, const struct console_opts *o,
    struct selection *sel)
{

	int start = 0;
	if (o->skip_first_n > 0 && sel->explicit_select) {
		start = o->skip_first_n;
		if (start > sel->items.count)
			start = sel->items.count;
	}

	int targets = sel->items.count - start;
	int sources = 0;
	for (int i = start; i < sel->items.count; i++) {
		char **paths = NULL;
		int n = xcindex_target_sources(d->query, sel->items.items[i],
		    &paths);
		sources += n;
		for (int k = 0; k < n; k++)
			free(paths[k]);
		free(paths);
	}

	printf("--- Stats\n");
	printf("targets: %d\n", targets);
	printf("blueprints: %d\n", targets);
	printf("source files: %d\n", sources);
	return 0;
}

/* beep: Apple's driver beeps the terminal and prints nothing else.  The
 * terminal bell is a side effect of the REPL, not output, so this is a
 * no-op that succeeds. */
static int
cmd_beep(struct driver *d)
{
	return 0;
}

/*
 * help: the console help text.  Apple prints it after the workspace line
 * and succeeds.
 */
static int
cmd_help(struct driver *d)
{
	fputs(kConsoleHelp, stdout);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Engine-backed commands (recognised, not yet realised)              */
/* ------------------------------------------------------------------ */

/*
 * These are the actions that hand off to a build engine: they need a run
 * destination, a build description, or the index build settings, none of
 * which this driver realises.  They are recognised so that their names
 * are not reported as unknown actions, and so that the option and scheme
 * validation that runs in front of every command still applies to them.
 * The bodies report the same missing-prerequisite errors Apple reports
 * when the prerequisite is absent.
 */
/*
 * Did the command name a target set explicitly?  A bare -scheme does not
 * count.  The target-requiring actions want one of the four selectors, and
 * say so when none of them is present; a scheme on its own is not enough.
 */
static int
has_target_selection(const struct console_opts *o)
{
	return o->targets.count > 0 || o->targets_of_scheme != NULL ||
	    o->targets_not_of_scheme != NULL || o->all_targets;
}

/*
 * The build engine is XCBuild, which this build does not have.  Every
 * engine-backed action whose prerequisites *are* present lands here: the
 * command is recognised, the option and scheme validation in front of it
 * has already run, and the only thing missing is the engine.  The message
 * is deliberately not Apple's -- Apple would have gone on to do the work,
 * and inventing an Apple-shaped error would claim a parity that is not
 * there.
 */
static int
engine_unavailable(struct driver *d)
{
	print_error(d, "the build engine is not available in this build", 0);
	return 1;
}

static int
cmd_engine(struct driver *d, const struct console_opts *o)
{

	const char *action = o->action;

	if (strcmp(action, "print-destination") == 0) {
		if (o->destination == NULL || o->destination[0] == '\0') {
			print_error(d, "no active run destination, use '-destination'",
			    0);
			return 1;
		}
		if (o->scheme == NULL) {
			print_error(d, "no active scheme, use '-scheme'", 0);
			return 1;
		}
		print_error(d, "option 'destination' requires at least one "
		    "parameter of the form 'key=value'", 0);
		return engine_unavailable(d);
	}

	if (strcmp(action, "print-build-description") == 0 ||
	    strcmp(action, "print-build-description-targets") == 0) {
		if (o->build_description == NULL ||
		    o->build_description[0] == '\0') {
			print_error(d, "no active build description, use "
			    "'create-build-description' command", 0);
			return 1;
		}
		return engine_unavailable(d);
	}

	/*
	 * prepare, print-index-build-settings and index-files each insist on
	 * an explicit target set before anything else -- before the run
	 * destination and before the build description -- and say so with the
	 * hint, unlike the two above.
	 */
	if (strcmp(action, "prepare") == 0 ||
	    strcmp(action, "print-index-build-settings") == 0 ||
	    strcmp(action, "index-files") == 0) {
		if (!has_target_selection(o)) {
			print_error(d, "no targets specified, use '-target' or "
			    "'-targets-of-scheme' or '-targets-not-of-scheme' "
			    "or '-all-targets'", 1);
			return 1;
		}
		if (strcmp(action, "print-index-build-settings") == 0) {
			if (o->build_description == NULL ||
			    o->build_description[0] == '\0') {
				print_error(d, "no active build description, use "
				    "'create-build-description' command", 0);
				return 1;
			}
		}
		return engine_unavailable(d);
	}

	/*
	 * create-build-description is the odd one: with no target set named
	 * it goes ahead and builds everything, but naming one makes it want a
	 * run destination first -- the opposite of an action that needs a
	 * destination and then a target set.  Only the pinned part is
	 * mirrored; the build itself needs the engine.
	 */
	if (strcmp(action, "create-build-description") == 0) {
		if (has_target_selection(o) &&
		    (o->destination == NULL || o->destination[0] == '\0')) {
			print_error(d, "no active run destination, use '-destination'",
			    0);
			return 1;
		}
		return engine_unavailable(d);
	}

	print_error(d, "unknown action", 1);
	return 1;
}

/* Is @arg a console action Apple knows? */
static int
is_action(const char *arg)
{
	static const char *const actions[] = {
		"help", "quit", "beep", "prepare", "create-build-description",
		"print-build-description", "print-build-description-targets",
		"print-destination", "print-stats", "list-indexables",
		"list-schemes", "print-index-build-settings", "index-files",
		NULL
	};
	return in_list(actions, arg);
}

/* ------------------------------------------------------------------ */
/* Running one console command                                        */
/* ------------------------------------------------------------------ */

/*
 * Parse a console command's tokens and run it.  Returns the process exit
 * status for this command: 0 on success, 1 on any reported error.  A
 * *quit is set when the command was "quit", which ends the REPL without
 * printing the trailing newline an EOF prints.
 *
 * @arg silent - the line the tokens came from was empty, which is a no-op.
 * A line of nothing but spaces is *not* silent: it has no action to run,
 * and Apple reports that.
 */
static int
run_console_command(struct driver *d, int argc, char **argv, int silent,
    int *quit)
{
	struct console_opts o;
	const char *bad_scheme = NULL;
	const char *bad_target = NULL;
	struct selection sel;

	console_opts_init(&o);
	*quit = 0;

	/* An empty line is a silent no-op. */
	if (silent)
		return 0;

	/*
	 * One left-to-right pass does both jobs at once, because the two are
	 * entangled: a value option swallows the token after it, so that token
	 * must not be mistaken for a second action.  "--" aside, the first bare
	 * word is the action, a second bare word is an error, and every "-..."
	 * token is looked up here.
	 */
	int too_many_actions = 0;
	for (int i = 0; i < argc; i++) {
		const char *arg = argv[i];

		if (arg[0] != '-') {
			if (o.action == NULL)
				o.action = arg;
			else
				too_many_actions = 1;
			continue;
		}

		if (in_list(kFlagOptions, arg)) {
			if (strcmp(arg, "-all-targets") == 0) o.all_targets = 1;
			else if (strcmp(arg, "-output-path-only") == 0) o.output_path_only = 1;
			else if (strcmp(arg, "-use-build-manager") == 0) o.use_build_manager = 1;
			else if (strcmp(arg, "-prepare-dependencies") == 0) o.prepare_dependencies = 1;
			else if (strcmp(arg, "-high-priority") == 0) o.high_priority = 1;
			else if (strcmp(arg, "-v") == 0) o.verbose = 1;
			else if (strcmp(arg, "-quiet") == 0) o.quiet = 1;
			continue;
		}

		if (in_list(kValueOptions, arg)) {
			const char *v = option_value(argc, argv, &i);
			if (strcmp(arg, "-destination") == 0) o.destination = v;
			else if (strcmp(arg, "-scheme") == 0) o.scheme = v;
			else if (strcmp(arg, "-target") == 0) strvec_push(&o.targets, v);
			else if (strcmp(arg, "-targets-of-scheme") == 0) o.targets_of_scheme = v;
			else if (strcmp(arg, "-targets-not-of-scheme") == 0) o.targets_not_of_scheme = v;
			else if (strcmp(arg, "-ignore-list-path") == 0) o.ignore_list_path = v;
			else if (strcmp(arg, "-skip-first-n") == 0) {
				if (!parse_count(v, &o.skip_first_n))
					o.skip_first_n = -2;  /* marked invalid */
			}
			else if (strcmp(arg, "-build-description") == 0) o.build_description = v;
			else if (strcmp(arg, "-file") == 0) o.file = v;
			else if (strcmp(arg, "-configuration") == 0) o.configuration = v;
			else if (strcmp(arg, "-qos") == 0) o.qos = v;
			else if (strcmp(arg, "-j") == 0) {
				if (!parse_count(v, &o.jobs))
					o.jobs = -2;       /* marked invalid */
			}
			continue;
		}

		/*
		 * "--" ends the command.  On a command line the tool layer has
		 * already taken the separators, so this only happens on a REPL
		 * line, where the line holds one command: everything past the
		 * first "--" is not part of it, so a bare word there is not a
		 * second action.  The "--" itself is still an unrecognised
		 * token, so the command fails on it whether or not anything
		 * follows -- but a second action ahead of the "--" is the error
		 * reported, because that outranks an unknown argument.
		 */
		if (strcmp(arg, "--") == 0) {
			if (o.unknown_arg == NULL)
				o.unknown_arg = arg;
			break;
		}

		/* Unrecognised: remember the first, reported after the int
		 * and QoS checks. */
		if (o.unknown_arg == NULL)
			o.unknown_arg = arg;
	}

	/* Validation, in the fixed precedence. */

	/* 1. -qos. */
	if (o.qos != NULL && !qos_valid(o.qos)) {
		char msg[256];
		snprintf(msg, sizeof(msg),
		    "unknown QoS name '%s', expected one of:\n"
		    "    background\n    utility\n    default\n    userInitiated\n",
		    o.qos);
		print_error(d, msg, 1);
		strvec_free(&o.targets);
		return 1;
	}

	/* 2. integer options. */
	if (o.jobs == -2 || o.skip_first_n == -2) {
		fprintf(stderr, "error: The operation couldn’t be completed. "
		    "(xcindex_test.CommandLineArguments.InvalidArgumentValueError "
		    "error 1.)\n");
		strvec_free(&o.targets);
		return 1;
	}

	/* 3. a second action, which is a parse error and outranks an unknown
	 * argument but not either of the two above. */
	if (too_many_actions) {
		print_error(d, "more than one action specified", 1);
		strvec_free(&o.targets);
		return 1;
	}

	if (o.action == NULL) {
		print_error(d, "no action specified", 1);
		strvec_free(&o.targets);
		return 1;
	}

	/* 4. unknown argument. */
	if (o.unknown_arg != NULL) {
		char msg[256];
		snprintf(msg, sizeof(msg), "unknown argument: '%s'",
		    o.unknown_arg);
		print_error(d, msg, 1);
		strvec_free(&o.targets);
		return 1;
	}

	/* An unrecognised action is reported before the workspace is even
	 * needed, but after it is opened, matching "frobnicate". */
	if (!is_action(o.action)) {
		char msg[256];
		snprintf(msg, sizeof(msg), "unknown action '%s'", o.action);
		print_error(d, msg, 1);
		strvec_free(&o.targets);
		return 1;
	}

	/* 4. -scheme must resolve, on every command including help. */
	if (o.scheme != NULL) {
		int from_file = 0;
		if (!xcindex_scheme_resolve(d->query, o.scheme, &from_file)) {
			char msg[256];
			snprintf(msg, sizeof(msg),
			    "scheme with name '%s' was not found", o.scheme);
			print_error(d, msg, 0);
			strvec_free(&o.targets);
			return 1;
		}
	}

	/* quit ends the REPL. */
	if (strcmp(o.action, "quit") == 0) {
		*quit = 1;
		strvec_free(&o.targets);
		return 0;
	}

	/*
	 * The selection is built for the commands that act on targets.  That
	 * includes the engine-backed ones: they resolve -target and the two
	 * scheme selectors before they do anything else, so `prepare -target
	 * nope` reports the missing target and not the missing destination.
	 * list-schemes, help, beep and the three engine actions that only
	 * report a missing prerequisite (print-destination, print-build-
	 * description, print-build-description-targets) never resolve a
	 * selection, and a target they cannot use is simply ignored.
	 */
	int need_selection = strcmp(o.action, "list-indexables") == 0 ||
	    strcmp(o.action, "print-stats") == 0 ||
	    strcmp(o.action, "prepare") == 0 ||
	    strcmp(o.action, "create-build-description") == 0 ||
	    strcmp(o.action, "index-files") == 0 ||
	    strcmp(o.action, "print-index-build-settings") == 0;

	if (need_selection) {
		/* 5. -target and the scheme selectors are validated here, in
		 * the order build_selection reports them. */
		if (build_selection(d, &o, &sel, &bad_scheme, &bad_target) != 0) {
			char msg[256];
			if (bad_target != NULL) {
				snprintf(msg, sizeof(msg),
				    "target '%s' not found", bad_target);
				print_error(d, msg, 1);
			} else {
				snprintf(msg, sizeof(msg),
				    "scheme with name '%s' was not found",
				    bad_scheme);
				print_error(d, msg, 0);
			}
			strvec_free(&o.targets);
			return 1;
		}
	} else {
		sel.items.items = NULL;
		sel.items.count = 0;
		sel.items.capacity = 0;
		sel.explicit_select = 0;
	}

	/*
	 * The command parsed and its selection resolved, so "Skipping N
	 * targets" can go out -- Apple emits it here, at argument-parse time,
	 * which is why it leads the load line even when a later command group
	 * carries the selector.  A group whose arguments did not get this far
	 * never prints it.
	 */
	if (need_selection && sel.explicit_select &&
	    o.skip_first_n >= 0)
		printf("Skipping %d targets\n", o.skip_first_n);

	/*
	 * The load line follows the skip line and precedes the report, and
	 * appears once however many commands run.
	 */
	ensure_loaded(d);

	/* Run the command. */
	struct timespec t0, t1;
	double elapsed;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	int rc;
	if (strcmp(o.action, "list-schemes") == 0)
		rc = cmd_list_schemes(d);
	else if (strcmp(o.action, "list-indexables") == 0)
		rc = cmd_list_indexables(d, &o, &sel);
	else if (strcmp(o.action, "print-stats") == 0)
		rc = cmd_print_stats(d, &o, &sel);
	else if (strcmp(o.action, "beep") == 0)
		rc = cmd_beep(d);
	else if (strcmp(o.action, "help") == 0)
		rc = cmd_help(d);
	else
		rc = cmd_engine(d, &o);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed = (double)(t1.tv_sec - t0.tv_sec) +
	    (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

	if (need_selection)
		strvec_free(&sel.items);
	strvec_free(&o.targets);

	if (rc != 0)
		return 1;
	/* A blank line separates the report from the closing line.  -quiet
	 * does not suppress it: the oracle still prints the timing line, and
	 * only the report itself is silenced. */
	printf("\n");
	print_seconds("command succeeded in ", elapsed);
	return 0;
}

/* ------------------------------------------------------------------ */
/* The command line and the REPL                                       */
/* ------------------------------------------------------------------ */

/*
 * Split a line on spaces, in place.  The separator is a space and only a
 * space: a tab is an ordinary character that happens to be inside a token,
 * so a line holding nothing but a tab is one (unknown) action, not an
 * empty command.  That is why a line of spaces yields no tokens while a
 * line of a single tab yields one.
 */
static int
tokenize(char *line, char **argv, int max)
{
	int argc = 0;
	char *p = line;

	while (*p != '\0' && argc < max) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		argv[argc++] = p;
		while (*p != '\0' && *p != ' ')
			p++;
		if (*p != '\0')
			*p++ = '\0';
	}
	return argc;
}

/*
 * Run the console commands given on the command line.  Each "--" starts a
 * new command; tokens between two "--" form one command.  Apple stops at
 * the first command that reports an error and returns 1; the remaining
 * commands are not run.
 */
static int
run_command_line_commands(struct driver *d, int argc, char **argv)
{
	char *group[256];
	int n = 0;
	int rc = 0;
	int quit = 0;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			if (n > 0) {
				int r = run_console_command(d, n, group, 0,
				    &quit);
				if (r != 0)
					return r;   /* stop at the first error */
			}
			n = 0;
			continue;
		}
		if (n < (int)(sizeof(group) / sizeof(group[0])))
			group[n++] = argv[i];
	}
	if (n > 0) {
		rc = run_console_command(d, n, group, 0, &quit);
	}

	return rc;
}

/*
 * The REPL: read a line, run it as a console command, repeat.  An empty
 * line is a no-op; a whitespace-only line is an error ("no action").  A
 * "quit" ends the session without the trailing newline; EOF prints one
 * blank line and ends.  The exit status is that of the last command that
 * ran.
 */
static int
run_repl(struct driver *d)
{
	char *line = NULL;
	size_t cap = 0;
	int rc = 0;
	int ended_by_quit = 0;

	/* d->repl is set so the error hints use the REPL wording. */
	d->repl = 1;

	while (1) {
		ssize_t n = getline(&line, &cap, stdin);
		if (n < 0)
			break;                   /* EOF */
		/* Strip the trailing newline. */
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';

		char *argv[256];
		int argc = tokenize(line, argv, 256);
		/* Silence keys on the line being empty, not on the token
		 * count: a line of nothing but spaces has no tokens but is
		 * still "no action specified".  The trailing newline has
		 * already been stripped, so a zero-length @arg line is the
		 * bare newline case. */
		int silent = (argc == 0 && n == 0);
		int quit = 0;
		int r = run_console_command(d, argc, argv, silent, &quit);
		if (r != 0)
			rc = r;               /* last non-zero sticks */
		if (quit) {
			ended_by_quit = 1;
			break;                   /* quit: no trailing newline */
		}
	}

	free(line);
	/* Running out of input without ever reaching a command still reports
	 * the load, and it leads the blank line that ends the session. */
	ensure_loaded(d);
	/* EOF prints a single blank line; a "quit" does not, because the user
	 * ended the session deliberately rather than running out of input. */
	if (!ended_by_quit)
		printf("\n");
	return rc;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	struct driver d;
	const char *project = NULL;
	int project_seen = 0;
	int want_help = 0;
	int console_start = argc;
	const char *unknown_arg = NULL;
	const char *unknown_input = NULL;

	memset(&d, 0, sizeof(d));
	d.repl = 0;

	/*
	 * Tool options come before the first "--"; the console commands come
	 * after it.
	 *
	 * A value option takes the next token only when that token does not
	 * begin with '-', so `-project -bogus` leaves -project without a
	 * value rather than swallowing the next option.  There is no
	 * `-option=value` form: the whole token is an unknown argument.
	 * Tokens that are not recognised are collected rather than reported
	 * on the spot, because the checks below all run first.
	 */
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (strcmp(arg, "--") == 0) {
			console_start = i + 1;
			break;
		}
		if (strcmp(arg, "-project") == 0) {
			project_seen = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				project = argv[++i];
		} else if (strcmp(arg, "-derivedDataPath") == 0) {
			/* Accepted and ignored: it names where XCBuild would
			 * put derived data, and the deterministic commands do
			 * not act on it. */
			if (i + 1 < argc && argv[i + 1][0] != '-')
				i++;
		} else if (strcmp(arg, "-help") == 0 ||
		    strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			want_help = 1;
		} else if (strcmp(arg, "-debugActivityLog") == 0 ||
		    strcmp(arg, "-continue-after-errors") == 0) {
			/* Flags, also inert here. */
		} else if (arg[0] == '-') {
			/* Apple reports one of these from an unordered
			 * collection, so with several in the command line which
			 * one is named is not determined; the first is as good
			 * as any and is what a single unknown argument gets. */
			if (unknown_arg == NULL)
				unknown_arg = arg;
		} else if (unknown_input == NULL) {
			unknown_input = arg;
		}
	}

	/* -help prints the tool help and exits, without opening a project and
	 * without looking at anything else that was on the line. */
	if (want_help) {
		fputs(kToolHelp, stdout);
		return 0;
	}

	/*
	 * The project is required before the unknown arguments are reported:
	 * `xcindex-test -bogus` complains about the missing project, not
	 * about -bogus.  Supplying the option is enough; a value that is
	 * missing or empty opens no project and is not an error.
	 */
	if (!project_seen) {
		fprintf(stderr, "error: need path for '-project'\n");
		fprintf(stderr, "See -help for information about available "
		    "commands and options\n");
		return 1;
	}

	/* An unrecognised option outranks an unrecognised bare word, whatever
	 * order they were written in. */
	if (unknown_arg != NULL || unknown_input != NULL) {
		fprintf(stderr, "error: %s: '%s'\n",
		    unknown_arg != NULL ? "unknown argument" : "unknown input",
		    unknown_arg != NULL ? unknown_arg : unknown_input);
		fprintf(stderr, "See -help for information about available "
		    "commands and options\n");
		return 1;
	}

	/* Resolve the project to an absolute path: every path printed by
	 * list-indexables is absolute, and a relative -project would
	 * otherwise yield relative ids. */
	char abspath[2048];
	if (project != NULL && project[0] != '\0' &&
	    xc_abspath(project, abspath, sizeof(abspath)) == NULL) {
		fprintf(stderr, "error: need path for '-project'\n");
		fprintf(stderr, "See -help for information about available "
		    "commands and options\n");
		return 1;
	}
	if (project == NULL || project[0] == '\0')
		abspath[0] = '\0';
	d.project = abspath;
	d.query = project_is_bundle(abspath) ? d.project : NULL;

	/*
	 * A project that does not exist is reported after the load line, which
	 * Apple prints regardless: the open is attempted, and the failure is
	 * not what keeps the line from appearing.  The message names the path
	 * as the user wrote it, so a relative -project is echoed as given
	 * rather than as the absolute path it was resolved to.
	 *
	 * Only a path that claims to be a bundle is looked for.  Anything else
	 * -- a directory, a path to the pbxproj, a name that does not exist --
	 * is simply not a project, and is not an error any more than a
	 * non-existent one would be for a console command.
	 */
	if (d.query != NULL) {
		struct stat sb;
		if (stat(d.project, &sb) != 0) {
			const char *base = strrchr(project, '/');
			base = base ? base + 1 : project;
			ensure_loaded(&d);
			fprintf(stderr, "error: The file \xe2\x80\x9c%s\xe2\x80\x9d "
			    "doesn\xe2\x80\x99t exist.\n", base);
			return 1;
		}
	}

	/*
	 * The load line is written at the first command that needs the
	 * workspace, so a run that stops before one -- a command line error, a
	 * quit, or stdin already at EOF -- still shows it on the way out.
	 */
	int rc = run_command_line_commands(&d, argc - console_start,
	    argv + console_start);
	if (rc != 0) {
		ensure_loaded(&d);
		return rc;
	}

	/* After the command line, the tool enters the REPL and reads stdin.
	 * With no commands on the command line and stdin at EOF, it prints
	 * a single blank line and exits 0. */
	rc = run_repl(&d);
	ensure_loaded(&d);
	return rc;
}
