/* xcodebuild -- open source reimplementation of Apple's xcodebuild utility
 *
 * Project / workspace introspection implementation.
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
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "xcodebuild.h"
#include "project.h"
#include "sdkpath.h"
#include "xcpath.h"

static const char *pbxproj_name = "project.pbxproj";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *file_read_all(const char *path, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz < 0) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	char *buf = (char *)malloc((size_t)sz + 1);
	if (buf == NULL) {
		fclose(fp);
		return NULL;
	}
	size_t rd = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	buf[rd] = '\0';
	if (out_len)
		*out_len = rd;
	return buf;
}

static int endswith(const char *s, const char *suffix)
{
	size_t ls = strlen(s);
	size_t lss = strlen(suffix);
	return ls >= lss && strcmp(s + ls - lss, suffix) == 0;
}

static int is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ------------------------------------------------------------------ */
/* .pbxproj location + loading                                          */
/* ------------------------------------------------------------------ */

char *project_pbxproj_path(const char *project)
{
	if (project == NULL)
		return NULL;
	char full[PATH_MAX];
	if (endswith(project, ".pbxproj")) {
		snprintf(full, sizeof(full), "%s", project);
		return access(full, R_OK) == 0 ? strdup(full) : NULL;
	}
	if (endswith(project, ".xcodeproj")) {
		char candidate[PATH_MAX];
		snprintf(candidate, sizeof(candidate), "%s/%s", project, pbxproj_name);
		return access(candidate, R_OK) == 0 ? strdup(candidate) : NULL;
	}
	/* Treat as a directory containing project.pbxproj. */
	char candidate[PATH_MAX];
	snprintf(candidate, sizeof(candidate), "%s/%s", project, pbxproj_name);
	if (access(candidate, R_OK) == 0)
		return strdup(candidate);
	return NULL;
}


/* ------------------------------------------------------------------ */
/* Property list access                                                 */
/*                                                                      */
/* A .pbxproj is an OpenStep-format property list, and Apple's          */
/* xcodebuild reads it with CoreFoundation, which parses that format    */
/* directly.  This tree used to parse it by hand and would give up on   */
/* real projects -- a stock Xcode template reported "missing a          */
/* rootObject" that CF finds without trouble.                           */
/*                                                                      */
/* These wrappers keep the walking code reading the way it did: fetch   */
/* by key, index an array, ask for a string, with the type checked and  */
/* a NULL container tolerated at every step.                            */
/* ------------------------------------------------------------------ */

static CFTypeRef
pget(CFTypeRef dict, const char *key)
{
	CFStringRef k;
	CFTypeRef v;

	if (dict == NULL || CFGetTypeID(dict) != CFDictionaryGetTypeID())
		return NULL;

	if ((k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8)) == NULL)
		return NULL;

	v = CFDictionaryGetValue((CFDictionaryRef)dict, k);
	CFRelease(k);
	return v;
}

static CFTypeRef
pat(CFTypeRef array, CFIndex i)
{
	if (array == NULL || CFGetTypeID(array) != CFArrayGetTypeID())
		return NULL;
	if (i < 0 || i >= CFArrayGetCount((CFArrayRef)array))
		return NULL;

	return CFArrayGetValueAtIndex((CFArrayRef)array, i);
}

static CFIndex
pcount(CFTypeRef array)
{
	if (array == NULL || CFGetTypeID(array) != CFArrayGetTypeID())
		return 0;

	return CFArrayGetCount((CFArrayRef)array);
}

static int
pis_dict(CFTypeRef v)
{
	return v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID();
}

/*
 * A property list string as C.  Copies into the caller's buffer, since
 * a CFString need not hold one -- returns NULL for anything that is not
 * a string, which is what every caller checks.
 */
static const char *
pstr(CFTypeRef v, char *buf, size_t len)
{
	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return NULL;
	if (!CFStringGetCString((CFStringRef)v, buf, (CFIndex)len,
	    kCFStringEncodingUTF8))
		return NULL;

	return buf;
}

/* Member of objects named by the string at key, when both are present. */
static CFTypeRef
pderef(CFTypeRef objects, CFTypeRef id_value)
{
	char id[512];

	if (pstr(id_value, id, sizeof(id)) == NULL)
		return NULL;

	return pget(objects, id);
}

CFTypeRef project_load_pbxproj(const char *project)
{
	char *path = project_pbxproj_path(project);
	if (path == NULL)
		return NULL;
	size_t len = 0;
	char *text = file_read_all(path, &len);
	free(path);
	if (text == NULL)
		return NULL;

	CFDataRef data = CFDataCreate(NULL, (const UInt8 *)text, (CFIndex)len);
	free(text);
	if (data == NULL)
		return NULL;

	CFPropertyListRef root = CFPropertyListCreateWithData(NULL, data,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);

	if (root != NULL && !pis_dict(root)) {
		CFRelease(root);
		return NULL;
	}

	return root;
}

/* ------------------------------------------------------------------ */
/* Build-settings extraction                                            */
/* ------------------------------------------------------------------ */

static CFTypeRef get_objects_dict(CFTypeRef root, CFTypeRef *out_root_obj_id)
{
	*out_root_obj_id = NULL;
	if (!pis_dict(root))
		return NULL;

	CFTypeRef objects = pget(root, "objects");
	if (!pis_dict(objects))
		return NULL;

	CFTypeRef root_id = pget(root, "rootObject");
	if (root_id == NULL)
		return NULL;

	*out_root_obj_id = root_id;
	return objects;
}

CFTypeRef project_get_project_object(CFTypeRef root)
{
	CFTypeRef root_id = NULL;
	CFTypeRef objects = get_objects_dict(root, &root_id);

	if (objects == NULL || root_id == NULL)
		return NULL;

	return pderef(objects, root_id);
}

CFTypeRef project_find_buildsettings(CFTypeRef root, const char *target,
                                     const char *configuration,
                                     char *chosen_name, size_t chosen_len)
{
	CFTypeRef root_id = NULL;
	CFTypeRef objects = get_objects_dict(root, &root_id);
	char name_buf[512];

	if (objects == NULL || root_id == NULL)
		return NULL;

	CFTypeRef project_obj = pderef(objects, root_id);
	if (!pis_dict(project_obj))
		return NULL;

	CFTypeRef targets = pget(project_obj, "targets");
	CFTypeRef chosen = NULL;

	if (chosen_name != NULL && chosen_len > 0)
		chosen_name[0] = '\0';

	for (CFIndex i = 0; i < pcount(targets); i++) {
		CFTypeRef tobj = pderef(objects, pat(targets, i));

		if (!pis_dict(tobj))
			continue;

		const char *name = pstr(pget(tobj, "name"), name_buf,
		    sizeof(name_buf));

		if (target != NULL && name != NULL &&
		    strcmp(name, target) == 0) {
			chosen = tobj;
			break;
		}
		if (target == NULL && chosen == NULL)
			chosen = tobj;
	}
	if (chosen == NULL)
		return NULL;

	/*
	 * Report which target the settings came from.  With no -target
	 * xcodebuild takes the first, and TARGET_NAME has to say so --
	 * PRODUCT_NAME and the rest are written as $(TARGET_NAME) and
	 * expand to nothing without it.
	 */
	if (chosen_name != NULL && chosen_len > 0) {
		const char *n = pstr(pget(chosen, "name"), name_buf,
		    sizeof(name_buf));

		if (n != NULL)
			snprintf(chosen_name, chosen_len, "%s", n);
	}

	CFTypeRef clist = pderef(objects, pget(chosen, "buildConfigurationList"));
	if (!pis_dict(clist))
		return NULL;

	CFTypeRef configs = pget(clist, "buildConfigurations");
	CFTypeRef first_cfg = NULL;

	for (CFIndex i = 0; i < pcount(configs); i++) {
		CFTypeRef cfg = pderef(objects, pat(configs, i));

		if (!pis_dict(cfg))
			continue;

		const char *cname = pstr(pget(cfg, "name"), name_buf,
		    sizeof(name_buf));

		if (cname == NULL)
			continue;
		if (first_cfg == NULL)
			first_cfg = cfg;
		if (configuration != NULL && strcmp(cname, configuration) == 0)
			return pget(cfg, "buildSettings");
	}

	if (configuration == NULL && first_cfg != NULL)
		return pget(first_cfg, "buildSettings");

	return NULL;
}

/* ------------------------------------------------------------------ */
/* String collection helpers (for listing)                              */
/* ------------------------------------------------------------------ */

typedef struct {
	char **items;
	size_t count;
	size_t capacity;
} strvec;

static int strvec_push(strvec *v, const char *s)
{
	if (v->count == v->capacity) {
		size_t cap = v->capacity ? v->capacity * 2 : 8;
		char **items = (char **)realloc(v->items, sizeof(char *) * cap);
		if (items == NULL)
			return -1;
		v->items = items;
		v->capacity = cap;
	}
	v->items[v->count++] = strdup(s);
	if (v->items[v->count - 1] == NULL)
		return -1;
	return 0;
}

static int strvec_present(strvec *v, const char *s)
{
	for (size_t i = 0; i < v->count; i++)
		if (strcmp(v->items[i], s) == 0)
			return 1;
	return 0;
}

static int strvec_push_unique(strvec *v, const char *s)
{
	if (strvec_present(v, s))
		return 0;
	return strvec_push(v, s);
}

/* Scheme names sort without regard to case, as xcodebuild prints them. */
static int scheme_name_cmp(const void *a, const void *b)
{
	return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void strvec_free(strvec *v)
{
	for (size_t i = 0; i < v->count; i++)
		free(v->items[i]);
	free(v->items);
	v->items = NULL;
	v->count = v->capacity = 0;
}

/* ------------------------------------------------------------------ */
/* Listing                                                             */
/* ------------------------------------------------------------------ */

static void collect_all_config_names(CFTypeRef objects, CFTypeRef target_ids,
                                     strvec *out)
{
	char name_buf[512];

	if (objects == NULL || target_ids == NULL)
		return;

	/* Collect from each target's buildConfigurationList. */
	for (CFIndex i = 0; i < pcount(target_ids); i++) {
		CFTypeRef tobj = pderef(objects, pat(target_ids, i));
		CFTypeRef clist = pderef(objects,
		    pget(tobj, "buildConfigurationList"));
		CFTypeRef configs = pget(clist, "buildConfigurations");

		for (CFIndex j = 0; j < pcount(configs); j++) {
			CFTypeRef cfg = pderef(objects, pat(configs, j));
			const char *cname = pstr(pget(cfg, "name"), name_buf,
			    sizeof(name_buf));

			if (cname != NULL)
				strvec_push_unique(out, cname);
		}
	}
}

/*
 * Targets of the projects this one references.
 *
 * A project may embed others through projectReferences, and Xcode
 * creates a scheme for each of their targets too -- so xcodebuild -list
 * shows them under Schemes while Targets stays the main project's own.
 * The referenced path is relative to the directory holding the
 * .xcodeproj, which is what it is resolved against here.
 */
static void
collect_subproject_targets(CFTypeRef objects, CFTypeRef project_obj,
    const char *project_path, strvec *out)
{
	CFTypeRef refs = pget(project_obj, "projectReferences");
	char dir[PATH_MAX], name_buf[512];
	const char *slash;

	if (refs == NULL || project_path == NULL)
		return;

	snprintf(dir, sizeof(dir), "%s", project_path);
	if ((slash = strrchr(dir, '/')) != NULL)
		*(char *)slash = '\0';
	else
		snprintf(dir, sizeof(dir), ".");

	for (CFIndex i = 0; i < pcount(refs); i++) {
		CFTypeRef entry = pat(refs, i);
		CFTypeRef fileref = pderef(objects, pget(entry, "ProjectRef"));
		const char *rel = pstr(pget(fileref, "path"), name_buf,
		    sizeof(name_buf));
		char sub[PATH_MAX];
		CFTypeRef subroot, subobjects, subproj, subtargets;
		CFTypeRef subroot_id = NULL;

		if (rel == NULL)
			continue;

		if (rel[0] == '/')
			snprintf(sub, sizeof(sub), "%s", rel);
		else
			snprintf(sub, sizeof(sub), "%s/%s", dir, rel);

		if ((subroot = project_load_pbxproj(sub)) == NULL)
			continue;

		subobjects = get_objects_dict(subroot, &subroot_id);
		subproj = pderef(subobjects, subroot_id);
		subtargets = pget(subproj, "targets");

		for (CFIndex j = 0; j < pcount(subtargets); j++) {
			CFTypeRef tobj = pderef(subobjects, pat(subtargets, j));
			char tbuf[512];
			const char *tname = pstr(pget(tobj, "name"), tbuf,
			    sizeof(tbuf));

			if (tname != NULL)
				strvec_push_unique(out, tname);
		}

		CFRelease(subroot);
	}
}

/*
 * The project's own build settings for a configuration.
 *
 * Xcode resolves a setting by inheritance: the project's configuration
 * first, then the target's on top.  Merging only the target's loses
 * everything set once for the whole project -- header search paths,
 * most often, which is why sources that include their own headers
 * failed to compile.
 */
CFTypeRef project_find_project_buildsettings(CFTypeRef root,
    const char *configuration)
{
	CFTypeRef root_id = NULL;
	CFTypeRef objects = get_objects_dict(root, &root_id);
	CFTypeRef project_obj = pderef(objects, root_id);
	CFTypeRef clist = pderef(objects,
	    pget(project_obj, "buildConfigurationList"));
	CFTypeRef configs = pget(clist, "buildConfigurations");
	CFTypeRef first = NULL;
	char name_buf[512];
	CFIndex i;

	for (i = 0; i < pcount(configs); i++) {
		CFTypeRef cfg = pderef(objects, pat(configs, i));
		const char *cname = pstr(pget(cfg, "name"), name_buf,
		    sizeof(name_buf));

		if (cname == NULL)
			continue;
		if (first == NULL)
			first = cfg;
		if (configuration != NULL && strcmp(cname, configuration) == 0)
			return pget(cfg, "buildSettings");
	}

	return (configuration == NULL && first != NULL) ?
	    pget(first, "buildSettings") : NULL;
}

/*
 * The configuration a build uses when the command line names none.
 *
 * The project's own configuration list says which, and saying so is
 * worth honouring: a project that defines only Debug means that, and
 * answering "Release" would name a configuration that is not there.
 *
 * Returns NULL when the project says nothing, which is its right to do;
 * "Release" is then the documented default.  Caller frees nothing: the
 * result is `buf`.
 */
const char *project_default_configuration(CFTypeRef root, char *buf,
    size_t len)
{
	CFTypeRef root_id = NULL;
	CFTypeRef objects;
	CFTypeRef project_obj, clist;

	if (root == NULL || buf == NULL || len == 0)
		return NULL;

	objects = get_objects_dict(root, &root_id);
	project_obj = pderef(objects, root_id);
	clist = pderef(objects, pget(project_obj, "buildConfigurationList"));
	if (clist == NULL)
		return NULL;

	return pstr(pget(clist, "defaultConfigurationName"), buf, len);
}

/* The productType of a target: what it builds. */
void project_target_product_type(CFTypeRef root, const char *target,
    char *buf, size_t len)
{
	CFTypeRef root_id = NULL;
	CFTypeRef objects = get_objects_dict(root, &root_id);
	CFTypeRef project_obj = pderef(objects, root_id);
	CFTypeRef targets = pget(project_obj, "targets");
	char name_buf[512];
	CFIndex i;

	if (buf == NULL || len == 0)
		return;
	buf[0] = '\0';

	for (i = 0; i < pcount(targets); i++) {
		CFTypeRef tobj = pderef(objects, pat(targets, i));
		const char *name = pstr(pget(tobj, "name"), name_buf,
		    sizeof(name_buf));

		if (target != NULL && (name == NULL || strcmp(name, target) != 0))
			continue;

		if (pstr(pget(tobj, "productType"), buf, len) == NULL)
			buf[0] = '\0';
		return;
	}
}

/* ------------------------------------------------------------------ */
/* schemes                                                              */
/* ------------------------------------------------------------------ */

/*
 * The value of an XML attribute, between `from` and `to`.
 *
 * A scheme is XML rather than a property list, so CoreFoundation is no
 * help here.  Only attributes are needed, and Xcode writes them one to
 * a line as `Name = "value"`, so this looks for the name and takes what
 * is inside the quotes after it.
 */
static const char *
xml_attr(const char *from, const char *to, const char *name, char *buf,
    size_t len)
{
	size_t nlen = strlen(name);
	const char *p = from;

	while (p != NULL && p < to && (p = strstr(p, name)) != NULL && p < to) {
		const char *q = p + nlen;
		size_t n = 0;

		/* Only a whole attribute name, not the tail of a longer one. */
		if (p > from && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
			p = q;
			continue;
		}

		while (q < to && (*q == ' ' || *q == '\t'))
			q++;
		if (q >= to || *q != '=') {
			p = q;
			continue;
		}
		q++;
		while (q < to && (*q == ' ' || *q == '\t'))
			q++;
		if (q >= to || *q != '"') {
			p = q;
			continue;
		}
		q++;

		while (q < to && *q != '"' && n + 1 < len)
			buf[n++] = *q++;
		buf[n] = '\0';

		return buf;
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* workspaces                                                           */
/* ------------------------------------------------------------------ */

/*
 * The projects a workspace refers to.
 *
 * contents.xcworkspacedata is XML listing a FileRef per project, which
 * Groups may nest.  A location carries the scheme it is measured from:
 * "group:" is relative to the enclosing group, "container:" and
 * "self:" to the workspace itself, and "absolute:" is already whole.
 *
 * Returns the number found; the caller frees each and the array.
 */
int
workspace_projects(const char *workspace, char ***paths)
{
	char base[PATH_MAX], stack[16][PATH_MAX];
	char file[PATH_MAX], *text, **list = NULL;
	const char *p;
	int depth = 0, count = 0;
	long len;
	FILE *fp;

	*paths = NULL;
	if (workspace == NULL)
		return 0;

	/*
	 * Paths are measured from the directory holding the workspace,
	 * not from the workspace itself: a bundle sitting beside the
	 * projects it refers to is not above them.
	 */
	snprintf(base, sizeof(base), "%s", workspace);
	{
		char *slash = strrchr(base, '/');

		if (slash != NULL)
			*slash = '\0';
		else
			snprintf(base, sizeof(base), ".");
	}

	snprintf(stack[0], sizeof(stack[0]), "%s", base);

	snprintf(file, sizeof(file), "%s/contents.xcworkspacedata", workspace);
	if ((fp = fopen(file, "rb")) == NULL)
		return 0;

	if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0) {
		fclose(fp);
		return 0;
	}
	rewind(fp);

	if ((text = malloc((size_t)len + 1)) == NULL) {
		fclose(fp);
		return 0;
	}
	if (len > 0 && fread(text, 1, (size_t)len, fp) != (size_t)len) {
		free(text);
		fclose(fp);
		return 0;
	}
	text[len] = '\0';
	fclose(fp);

	for (p = text; *p != '\0'; p++) {
		const char *tag_end;
		char loc[PATH_MAX], resolved[PATH_MAX];
		const char *kind, *rest;
		int is_group;

		if (*p != '<')
			continue;

		if (strncmp(p, "</Group", 7) == 0) {
			if (depth > 0)
				depth--;
			continue;
		}

		is_group = (strncmp(p, "<Group", 6) == 0);
		if (!is_group && strncmp(p, "<FileRef", 8) != 0)
			continue;

		if ((tag_end = strchr(p, '>')) == NULL)
			break;

		if (xml_attr(p, tag_end, "location", loc, sizeof(loc)) == NULL) {
			if (is_group && depth + 1 < 16) {
				snprintf(stack[depth + 1],
				    sizeof(stack[0]), "%s", stack[depth]);
				depth++;
			}
			continue;
		}

		/* The part before the colon says what the path is from. */
		if ((rest = strchr(loc, ':')) != NULL) {
			kind = loc;
			*(char *)rest = '\0';
			rest++;
		} else {
			kind = "group";
			rest = loc;
		}

		if (strcmp(kind, "absolute") == 0 || rest[0] == '/')
			snprintf(resolved, sizeof(resolved), "%s", rest);
		else if (strcmp(kind, "container") == 0 ||
		    strcmp(kind, "self") == 0)
			snprintf(resolved, sizeof(resolved), "%s/%s", base,
			    rest);
		else
			snprintf(resolved, sizeof(resolved), "%s/%s",
			    stack[depth], rest);

		if (is_group) {
			if (depth + 1 < 16) {
				snprintf(stack[depth + 1], sizeof(stack[0]),
				    "%s", resolved);
				depth++;
			}
			continue;
		}

		{
			char **grown = realloc(list,
			    (size_t)(count + 1) * sizeof(*list));

			if (grown != NULL) {
				list = grown;
				if ((list[count] = strdup(resolved)) != NULL)
					count++;
			}
		}
	}

	free(text);
	*paths = list;

	return count;
}

/* Where a scheme of this name lives, shared or belonging to a user. */
static int
scheme_file(const char *project, const char *scheme, char *out, size_t len)
{
	char dir[PATH_MAX];
	struct dirent *e;
	struct stat st;
	DIR *d;

	snprintf(out, len, "%s/xcshareddata/xcschemes/%s.xcscheme", project,
	    scheme);
	if (stat(out, &st) == 0)
		return 1;

	snprintf(dir, sizeof(dir), "%s/xcuserdata", project);
	if ((d = opendir(dir)) == NULL)
		return 0;

	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;

		snprintf(out, len, "%s/%s/xcschemes/%s.xcscheme", dir,
		    e->d_name, scheme);

		if (stat(out, &st) == 0) {
			closedir(d);
			return 1;
		}
	}

	closedir(d);
	return 0;
}

/*
 * The configuration a build uses when the project has shared schemes: the
 * buildConfiguration of the first scheme's LaunchAction.
 *
 * -showBuildSettingsForIndex takes the configuration from the scheme, and
 * the scheme outranks both the project's own defaultConfigurationName and
 * the record of an earlier index build.  The Sources fixture names Release
 * as its default and keeps a shared scheme whose LaunchAction says Debug,
 * and answers Debug; a project Xcode has already indexed once still
 * follows its scheme, so editing the scheme edits the answer even while
 * the record is sitting in DerivedData.
 *
 * Schemes are taken in name order and the first one decides: with
 * AAA.xcscheme and ZZZ.xcscheme disagreeing it is AAA, and writing ZZZ
 * into the directory first does not change that.  xcschememanagement.plist
 * does not override the choice.  LaunchAction alone is read -- moving the
 * configuration on TestAction, AnalyzeAction, ProfileAction,
 * ArchiveAction or BuildAction leaves the answer alone.
 *
 * NULL when the project has no shared scheme, which is the case that leaves
 * the caller to the record and the project's default.
 */
const char *
project_scheme_configuration(const char *project, char *buf, size_t len)
{
	char dir[PATH_MAX], path[PATH_MAX], first[256];
	const char *action, *action_end;
	struct dirent *e;
	size_t tlen;
	char *text;
	DIR *d;

	if (project == NULL || buf == NULL || len == 0)
		return NULL;

	snprintf(dir, sizeof(dir), "%s/xcshareddata/xcschemes", project);
	if ((d = opendir(dir)) == NULL)
		return NULL;

	/* readdir order is the order the files were written, which is not
	 * the order Xcode picks them in, so the name has to be compared. */
	first[0] = '\0';
	while ((e = readdir(d)) != NULL) {
		if (!endswith(e->d_name, ".xcscheme"))
			continue;
		if (first[0] != '\0' && strcmp(e->d_name, first) >= 0)
			continue;
		snprintf(first, sizeof(first), "%s", e->d_name);
	}
	closedir(d);

	if (first[0] == '\0')
		return NULL;

	snprintf(path, sizeof(path), "%s/%s", dir, first);
	if ((text = file_read_all(path, &tlen)) == NULL)
		return NULL;

	action = strstr(text, "<LaunchAction");
	action_end = (action != NULL) ? strstr(action, "</LaunchAction>") : NULL;
	if (action_end == NULL)
		action_end = text + tlen;

	if (xml_attr(action, action_end, "buildConfiguration", buf, len) == NULL)
		buf[0] = '\0';
	free(text);

	return buf[0] != '\0' ? buf : NULL;
}

/*
 * The targets a scheme builds, in the order it lists them.
 *
 * Only the build action counts: the other actions name what to run and
 * what to test, and a scheme's Testables are not built by `xcodebuild
 * build`.  An entry every buildFor... says NO to is skipped, as Xcode
 * skips it.
 *
 * Returns the number of names, or 0 when the scheme has no file --
 * Xcode creates one per target on demand and writes nothing to disk,
 * so a name with no file is simply the target of that name, which the
 * caller resolves.
 */
int
project_scheme_targets(const char *project, const char *scheme, char ***names,
    char ***containers)
{
	char path[PATH_MAX];
	char *text, **list = NULL, **clist = NULL;
	const char *action, *action_end, *p;
	long len;
	FILE *fp;
	int count = 0;

	*names = NULL;
	if (containers != NULL)
		*containers = NULL;

	if (project == NULL || scheme == NULL)
		return 0;
	if (!scheme_file(project, scheme, path, sizeof(path)))
		return 0;
	if ((fp = fopen(path, "rb")) == NULL)
		return 0;

	if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0) {
		fclose(fp);
		return 0;
	}
	rewind(fp);

	if ((text = malloc((size_t)len + 1)) == NULL) {
		fclose(fp);
		return 0;
	}
	if (len > 0 && fread(text, 1, (size_t)len, fp) != (size_t)len) {
		free(text);
		fclose(fp);
		return 0;
	}
	text[len] = '\0';
	fclose(fp);

	if ((action = strstr(text, "<BuildAction")) == NULL ||
	    (action_end = strstr(action, "</BuildAction>")) == NULL) {
		free(text);
		return 0;
	}

	for (p = action; (p = strstr(p, "<BuildActionEntry")) != NULL &&
	    p < action_end; ) {
		const char *entry_end = strstr(p, "</BuildActionEntry>");
		char buf[512];
		const char *name;
		char **grown;

		if (entry_end == NULL || entry_end > action_end)
			break;

		/* An entry no action builds is not built. */
		if (xml_attr(p, entry_end, "buildForRunning", buf,
		    sizeof(buf)) == NULL || strcmp(buf, "YES") != 0) {
			int wanted = 0;
			static const char *const also[] = {
				"buildForTesting", "buildForProfiling",
				"buildForArchiving", "buildForAnalyzing"
			};
			size_t i;

			for (i = 0; i < sizeof(also) / sizeof(also[0]); i++)
				if (xml_attr(p, entry_end, also[i], buf,
				    sizeof(buf)) != NULL &&
				    strcmp(buf, "YES") == 0)
					wanted = 1;

			if (!wanted) {
				p = entry_end;
				continue;
			}
		}

		name = xml_attr(p, entry_end, "BlueprintName", buf,
		    sizeof(buf));

		if (name != NULL && *name != '\0' &&
		    (grown = realloc(list, (size_t)(count + 1) *
		    sizeof(*list))) != NULL) {
			list = grown;

			if ((list[count] = strdup(name)) != NULL) {
				/*
				 * Which project the target is in.  A
				 * scheme in a workspace names targets of
				 * several, so an entry says which.
				 */
				if (containers != NULL) {
					char cbuf[PATH_MAX];
					const char *c = xml_attr(p, entry_end,
					    "ReferencedContainer", cbuf,
					    sizeof(cbuf));
					char **cg = realloc(clist,
					    (size_t)(count + 1) * sizeof(*cg));

					if (cg != NULL) {
						clist = cg;
						clist[count] = strdup(
						    (c != NULL) ? c : "");
					}
				}

				count++;
			}
		}

		p = entry_end;
	}

	free(text);
	*names = list;
	if (containers != NULL)
		*containers = clist;

	return count;
}

/*
 * Target ids Xcode was told not to autocreate a scheme for.
 *
 * xcschememanagement.plist records this under
 * SuppressBuildableAutocreation, keyed by target id.  It lives in a
 * user's xcuserdata and in xcshareddata; both are read, since either may
 * carry the setting.  The file is a property list of either flavour, so
 * CoreFoundation reads it.
 */
static void
merge_suppressed(const char *plist_path, strvec *out)
{
	CFDataRef data;
	CFPropertyListRef root;
	CFDictionaryRef suppress;
	char *text;
	size_t len;
	CFIndex n, i;
	const void **keys;

	if ((text = file_read_all(plist_path, &len)) == NULL)
		return;

	data = CFDataCreate(NULL, (const UInt8 *)text, (CFIndex)len);
	free(text);
	if (data == NULL)
		return;

	root = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable,
	    NULL, NULL);
	CFRelease(data);
	if (root == NULL)
		return;

	if (CFGetTypeID(root) == CFDictionaryGetTypeID()) {
		suppress = CFDictionaryGetValue((CFDictionaryRef)root,
		    CFSTR("SuppressBuildableAutocreation"));

		if (suppress != NULL &&
		    CFGetTypeID(suppress) == CFDictionaryGetTypeID()) {
			n = CFDictionaryGetCount(suppress);
			keys = calloc((size_t)n, sizeof(*keys));
			if (keys != NULL) {
				CFDictionaryGetKeysAndValues(suppress, keys, NULL);
				for (i = 0; i < n; i++) {
					char id[512];

					if (CFStringGetCString((CFStringRef)keys[i],
					    id, sizeof(id), kCFStringEncodingUTF8))
						strvec_push_unique(out, id);
				}
				free(keys);
			}
		}
	}

	CFRelease(root);
}

static void
collect_suppressed_targets(const char *project, strvec *out)
{
	char path[PATH_MAX], userdata[PATH_MAX];
	DIR *d;
	struct dirent *e;

	/* Shared. */
	snprintf(path, sizeof(path),
	    "%s/xcshareddata/xcschemes/xcschememanagement.plist", project);
	merge_suppressed(path, out);

	/* Every user's, since the project may be anyone's checkout. */
	snprintf(userdata, sizeof(userdata), "%s/xcuserdata", project);
	if ((d = opendir(userdata)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path),
			    "%s/%s/xcschemes/xcschememanagement.plist",
			    userdata, e->d_name);
			merge_suppressed(path, out);
		}
		closedir(d);
	}
}

/*
 * A project's name is its bundle's, minus the extension: PBXProject
 * carries no "name" of its own.
 */
void project_display_name(const char *path, char *buf, size_t len)
{
	const char *slash;
	char *dot;

	if (buf == NULL || len == 0)
		return;

	buf[0] = '\0';
	if (path == NULL)
		return;

	slash = strrchr(path, '/');
	snprintf(buf, len, "%s", (slash != NULL) ? slash + 1 : path);
	if ((dot = strrchr(buf, '.')) != NULL && dot != buf)
		*dot = '\0';
}

/*
 * xcodebuild -list for a workspace.
 *
 * A workspace has no targets and no configurations of its own -- it
 * refers to projects that have them -- so only schemes are listed:
 * its own shared ones, and then everything each project it refers to
 * would report, which is that project's shared schemes and one per
 * target.
 */
static int
workspace_list(const char *workspace)
{
	char name[PATH_MAX], sdir[PATH_MAX];
	char **projects = NULL;
	strvec schemes = {0};
	const char *slash;
	struct dirent *e;
	int nprojects, i;
	DIR *d;

	snprintf(name, sizeof(name), "%s", workspace);
	if ((slash = strrchr(name, '/')) != NULL)
		memmove(name, slash + 1, strlen(slash + 1) + 1);
	{
		char *dot = strstr(name, ".xcworkspace");

		if (dot != NULL)
			*dot = '\0';
	}

	snprintf(sdir, sizeof(sdir), "%s/xcshareddata/xcschemes", workspace);
	if ((d = opendir(sdir)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			char buf[256];
			char *dot;

			if (!endswith(e->d_name, ".xcscheme"))
				continue;

			snprintf(buf, sizeof(buf), "%s", e->d_name);
			if ((dot = strstr(buf, ".xcscheme")) != NULL)
				*dot = '\0';
			strvec_push_unique(&schemes, buf);
		}
		closedir(d);
	}

	nprojects = workspace_projects(workspace, &projects);

	for (i = 0; i < nprojects; i++) {
		CFTypeRef root, objects, project_obj, tarr;
		strvec suppressed = {0};
		char pdir[PATH_MAX];
		CFIndex ti;

		snprintf(pdir, sizeof(pdir), "%s/xcshareddata/xcschemes",
		    projects[i]);
		if ((d = opendir(pdir)) != NULL) {
			while ((e = readdir(d)) != NULL) {
				char buf[256];
				char *dot;

				if (!endswith(e->d_name, ".xcscheme"))
					continue;

				snprintf(buf, sizeof(buf), "%s", e->d_name);
				if ((dot = strstr(buf, ".xcscheme")) != NULL)
					*dot = '\0';
				strvec_push_unique(&schemes, buf);
			}
			closedir(d);
		}

		collect_suppressed_targets(projects[i], &suppressed);

		if ((root = project_load_pbxproj(projects[i])) != NULL) {
			objects = pget(root, "objects");
			project_obj = project_get_project_object(root);
			tarr = pget(project_obj, "targets");

			for (ti = 0; ti < pcount(tarr); ti++) {
				CFTypeRef tid = pat(tarr, ti);
				char idbuf[512], tnbuf[512];
				const char *id = pstr(tid, idbuf, sizeof(idbuf));
				const char *tn = pstr(pget(pderef(objects, tid),
				    "name"), tnbuf, sizeof(tnbuf));

				if (tn == NULL)
					continue;
				if (id != NULL && strvec_present(&suppressed, id))
					continue;

				strvec_push_unique(&schemes, tn);
			}

			CFRelease(root);
		}

		strvec_free(&suppressed);
		free(projects[i]);
	}
	free(projects);

	printf("Information about workspace \"%s\":\n", name);

	if (schemes.count > 0) {
		size_t k;

		qsort(schemes.items, schemes.count, sizeof(*schemes.items),
		    scheme_name_cmp);

		printf("    Schemes:\n");
		for (k = 0; k < schemes.count; k++)
			printf("        %s\n", schemes.items[k]);
		printf("\n");
	}

	strvec_free(&schemes);

	return 0;
}

int project_list(const char *project, const char *workspace, const xcodebuild_opts *opts)
{
	(void)opts;
	CFTypeRef root;

	/* A workspace has projects rather than targets of its own. */
	if (project == NULL && workspace != NULL)
		return workspace_list(workspace);

	root = project_load_pbxproj(project ? project : workspace);
	char name_buf[512];

	if (root == NULL) {
		fprintf(stderr, "xcodebuild: error: could not find project at '%s'\n",
		        project ? project : (workspace ? workspace : "."));
		return 1;
	}

	CFTypeRef root_id = NULL;
	CFTypeRef objects = get_objects_dict(root, &root_id);
	if (objects == NULL || root_id == NULL) {
		fprintf(stderr, "xcodebuild: error: project is missing a rootObject\n");
		CFRelease(root);
		return 1;
	}

	CFTypeRef project_obj = pderef(objects, root_id);
	if (project_obj == NULL) {
		CFRelease(root);
		return 1;
	}

	/*
	 * The project's name is its bundle's, not a key in the file: a
	 * PBXProject carries no "name", so reading one there left every
	 * real project reported as "project".
	 */
	const char *proj_name = pstr(pget(project_obj, "name"), name_buf,
	    sizeof(name_buf));

	if (proj_name == NULL) {
		project_display_name(project ? project : workspace, name_buf,
		    sizeof(name_buf));
		if (name_buf[0] != '\0')
			proj_name = name_buf;
	}

	printf("Information about project \"%s\":\n",
	    (proj_name != NULL) ? proj_name : "project");

	/* Targets. */
	strvec targets = {0};
	CFTypeRef tarr = pget(project_obj, "targets");
	for (CFIndex i = 0; i < pcount(tarr); i++) {
		CFTypeRef tobj = pderef(objects, pat(tarr, i));
		const char *tname = pstr(pget(tobj, "name"), name_buf,
		    sizeof(name_buf));

		if (tname != NULL)
			strvec_push(&targets, tname);
	}
	if (targets.count > 0) {
		printf("    Targets:\n");
		for (size_t i = 0; i < targets.count; i++)
			printf("        %s\n", targets.items[i]);
		printf("\n");
	}

	/*
	 * Build configurations, with the note xcodebuild prints about
	 * which one it will pick.  The blank lines and the wording are
	 * part of the output anything parsing -list has to read.
	 */
	strvec configs = {0};
	char defbuf[128];
	const char *defcfg = "Release";

	/*
	 * Which configuration a build without -configuration uses is the
	 * project's to say: its configuration list names one.  Reading it
	 * matters for a project that defines only Debug, where saying
	 * "Release" names a configuration that is not there.
	 *
	 * This is the same question a build answers, and it asks it
	 * through project_default_configuration() so that -list and the
	 * build cannot come to disagree.
	 */
	{
		const char *d = project_default_configuration(root, defbuf,
		    sizeof(defbuf));

		if (d != NULL && *d != '\0')
			defcfg = d;
	}

	collect_all_config_names(objects, tarr, &configs);
	if (configs.count > 0) {
		printf("    Build Configurations:\n");
		for (size_t i = 0; i < configs.count; i++)
			printf("        %s\n", configs.items[i]);
		printf("\n");
		printf("    If no build configuration is specified and"
		    " -scheme is not passed then \"%s\" is used.\n", defcfg);
		printf("\n");
	}
	strvec_free(&configs);

	/*
	 * Schemes.  A project's shared schemes are files on disk, but
	 * Xcode also creates one per target on demand, and xcodebuild
	 * lists those too -- a project with a single shared scheme still
	 * reports one per target.  The two sets are merged, and sorted
	 * without regard to case, which is the order they are printed in.
	 */
	strvec schemes = {0};
	strvec suppressed = {0};
	char base[PATH_MAX], sdir[PATH_MAX];
	DIR *d;

	snprintf(base, sizeof(base), "%s",
	    project ? project : (workspace ? workspace : "."));
	collect_suppressed_targets(base, &suppressed);
	snprintf(sdir, sizeof(sdir), "%s/xcshareddata/xcschemes", base);

	if ((d = opendir(sdir)) != NULL) {
		struct dirent *e;

		while ((e = readdir(d)) != NULL) {
			char namebuf[256];
			char *dot;

			if (!endswith(e->d_name, ".xcscheme"))
				continue;

			snprintf(namebuf, sizeof(namebuf), "%s", e->d_name);
			if ((dot = strstr(namebuf, ".xcscheme")) != NULL)
				*dot = '\0';
			strvec_push_unique(&schemes, namebuf);
		}
		closedir(d);
	}

	/*
	 * A scheme per target, except targets Xcode was told not to
	 * autocreate one for.  That list is SuppressBuildableAutocreation
	 * in xcschememanagement.plist, keyed by target id -- which is why
	 * a test bundle appears for one project and not another: it is a
	 * per-project choice recorded there, not a property of the type.
	 */
	for (CFIndex ti = 0; ti < pcount(tarr); ti++) {
		CFTypeRef tid = pat(tarr, ti);
		CFTypeRef tobj = pderef(objects, tid);
		char idbuf[512], tnbuf[512];
		const char *id = pstr(tid, idbuf, sizeof(idbuf));
		const char *tn = pstr(pget(tobj, "name"), tnbuf, sizeof(tnbuf));

		if (tn == NULL)
			continue;
		if (id != NULL && strvec_present(&suppressed, id))
			continue;

		strvec_push_unique(&schemes, tn);
	}

	collect_subproject_targets(objects, project_obj, base, &schemes);

	printf("    Schemes:\n");
	if (schemes.count > 0) {
		qsort(schemes.items, schemes.count, sizeof(*schemes.items),
		    scheme_name_cmp);
		for (size_t i = 0; i < schemes.count; i++)
			printf("        %s\n", schemes.items[i]);
		printf("\n");
	} else {
		printf("        (no schemes found)\n");
	}

	strvec_free(&schemes);
	strvec_free(&suppressed);
	strvec_free(&targets);

	CFRelease(root);
	return 0;
}

/* ------------------------------------------------------------------ */
/* xcindex-test -- the model the third IDETools product exposes.       */
/*                                                                    */
/* xcindex-test and xcodebuild disagree about a project's schemes on  */
/* purpose.  xcodebuild asks xcschememanagement.plist which targets   */
/* must not get an on-demand scheme; xcindex-test instead suppresses  */
/* a target-derived name when some scheme FILE carries a              */
/* BuildableProductRunnable for that target, provided the reference's */
/* ReferencedContainer matches the project's own.  The two also sort  */
/* differently: xcodebuild folds case, xcindex-test uses byte order.  */
/* ------------------------------------------------------------------ */

/* Sort under strcmp, unlike the case-folding xcodebuild uses. */
static int
xcindex_name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Apple decodes a scheme into its typed actions and only reads the target
 * references those actions contain, so a BuildableReference counts when it
 * sits in a runnable wrapper -- BuildableProductRunnable for a launch or
 * profile action, BuildActionEntry for a build action, TestableReference for
 * a test action -- and that wrapper sits somewhere inside a known action.  A
 * reference under any other element is not part of the model and is ignored,
 * which is why a scan for every BuildableReference in the file finds more
 * targets than Apple does.
 */
static int
scheme_is_action(const char *name, size_t len)
{
	static const char *const actions[] = {
		"BuildAction", "LaunchAction", "TestAction", "ProfileAction",
		"AnalyzeAction", "ArchiveAction", "AnalyzeAndArchiveAction",
	};
	size_t i;

	for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
		if (strlen(actions[i]) == len &&
		    strncmp(actions[i], name, len) == 0)
			return 1;
	}
	return 0;
}

static int
scheme_is_runnable(const char *name, size_t len)
{
	static const char *const runnables[] = {
		"BuildableProductRunnable", "BuildActionEntry",
		"TestableReference", "MacroReference",
	};
	size_t i;

	for (i = 0; i < sizeof(runnables) / sizeof(runnables[0]); i++) {
		if (strlen(runnables[i]) == len &&
		    strncmp(runnables[i], name, len) == 0)
			return 1;
	}
	return 0;
}

/* Is the element the one that makes a target runnable by the scheme?  A
 * build entry or a test entry says the scheme builds or tests the target,
 * which is not the same as saying the scheme can run it. */
static int
scheme_is_launchable(const char *name, size_t len)
{
	return len == strlen("BuildableProductRunnable") &&
	    strncmp(name, "BuildableProductRunnable", len) == 0;
}

/*
 * The next BuildableReference Apple would read, as a span over the
 * reference's own tag.  The document is walked tag by tag keeping a stack
 * of the open elements, so the reference's parent and the action enclosing
 * it are both known when it is found.
 *
 * The walk always starts at the top of the document and skips past whatever
 * the caller has already consumed.  Resuming mid-file instead would lose
 * the open elements a reference sits inside -- a second BuildActionEntry
 * after the first would no longer know it was inside a BuildAction -- so
 * every reference but the first would be judged to be outside the model.
 */
static int
scheme_runnable_next(const char *text, const char **from, int launchable_only,
    const char **ref_start, const char **ref_end)
{
	struct {
		const char *name;
		size_t len;
	} stack[64];
	int depth = 0;
	const char *p = text;

	while ((p = strchr(p, '<')) != NULL) {
		const char *tag = p + 1, *e, *gt;
		size_t len;
		int empty;

		/* A comment, a declaration or a doctype carries no elements. */
		if (*tag == '?' || *tag == '!') {
			if ((p = strchr(tag, '>')) == NULL)
				return 0;
			p++;
			continue;
		}

		/* A closing tag pops the element it names. */
		if (*tag == '/') {
			if (depth > 0)
				depth--;
			if ((p = strchr(tag, '>')) == NULL)
				return 0;
			p++;
			continue;
		}

		for (e = tag; *e != '\0' && *e != ' ' && *e != '\t' &&
		    *e != '\n' && *e != '\r' && *e != '>' && *e != '/'; e++)
			;
		len = (size_t)(e - tag);

		if ((gt = strchr(tag, '>')) == NULL)
			return 0;
		empty = (gt > tag && gt[-1] == '/');

		if (len == strlen("BuildableReference") &&
		    strncmp(tag, "BuildableReference", len) == 0) {
			int in_action = 0;
			int accepted;
			int i;

			for (i = 0; i < depth; i++) {
				if (scheme_is_action(stack[i].name,
				    stack[i].len))
					in_action = 1;
			}

			accepted = depth > 0 && in_action &&
			    (launchable_only
			    ? scheme_is_launchable(stack[depth - 1].name,
			        stack[depth - 1].len)
			    : scheme_is_runnable(stack[depth - 1].name,
			        stack[depth - 1].len));

			if (p >= *from && accepted) {
				*ref_start = p;
				*ref_end = gt;
				*from = gt + 1;
				return 1;
			}
		}

		if (!empty && depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
			stack[depth].name = tag;
			stack[depth].len = len;
			depth++;
		}
		p = gt + 1;
	}

	return 0;
}

/*
 * Whether a scheme file suppresses the target-derived scheme of the
 * target named by (guid, name): some runnable in it launches that target
 * from the project's own container.  Only a launch counts.  A scheme that
 * merely builds or tests a target leaves the target's own scheme in the
 * list, so a project whose scheme file has a BuildActionEntry naming a
 * target still offers that target's generated scheme alongside the file's.
 */
static int
xcindex_scheme_suppresses_target(const char *text, const char *guid,
    const char *name, const char *container)
{
	const char *from = text, *rs, *re;
	char gbuf[512], nbuf[512], bbuf[512], cbuf[512];
	const char *g, *n, *b, *c;
	int matched = 0;

	while (scheme_runnable_next(text, &from, 1, &rs, &re)) {
		g = xml_attr(rs, re, "BlueprintIdentifier", gbuf, sizeof(gbuf));
		n = xml_attr(rs, re, "BlueprintName", nbuf, sizeof(nbuf));
		b = xml_attr(rs, re, "BuildableName", bbuf, sizeof(bbuf));
		c = xml_attr(rs, re, "ReferencedContainer", cbuf, sizeof(cbuf));

		/*
		 * The reference resolves to the target when any of its
		 * identifiers does, and it is from this project when the
		 * container agrees.  Both together, or the scheme is not
		 * this target's.
		 */
		if (c != NULL && strcmp(c, container) == 0 &&
		    ((g != NULL && strcmp(g, guid) == 0) ||
		     (n != NULL && strcmp(n, name) == 0) ||
		     (b != NULL && strcmp(b, name) == 0))) {
			matched = 1;
			break;
		}
	}

	return matched;
}

/*
 * Every scheme file of the project, shared and each user's, without
 * regard to which "wins" for a name -- a variant of collect_suppressed
 * but for the file's text rather than xcschememanagement.plist.
 */
static void
xcindex_collect_suppressing_text(const char *project, const char *guid,
    const char *name, const char *container, int *suppressed)
{
	char base[PATH_MAX], dir[PATH_MAX];
	DIR *d;
	struct dirent *e;

	if (*suppressed)
		return;

	snprintf(base, sizeof(base), "%s", project);
	snprintf(dir, sizeof(dir), "%s/xcshareddata/xcschemes", base);

	if ((d = opendir(dir)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			char path[PATH_MAX], *text;
			size_t len;

			if (!endswith(e->d_name, ".xcscheme"))
				continue;

			snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
			if ((text = file_read_all(path, &len)) != NULL) {
				if (xcindex_scheme_suppresses_target(text,
				    guid, name, container))
					*suppressed = 1;
				free(text);
			}
		}
		closedir(d);
	}

	snprintf(dir, sizeof(dir), "%s/xcuserdata", base);
	if ((d = opendir(dir)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			char udir[PATH_MAX], path[PATH_MAX];
			char *text;
			size_t len;
			DIR *rd;
			struct dirent *r;

			if (e->d_name[0] == '.')
				continue;

			snprintf(udir, sizeof(udir), "%s/%s/xcschemes", dir,
			    e->d_name);
			if ((rd = opendir(udir)) == NULL)
				continue;

			while ((r = readdir(rd)) != NULL) {
				if (!endswith(r->d_name, ".xcscheme"))
					continue;

				snprintf(path, sizeof(path), "%s/%s", udir,
				    r->d_name);
				if ((text = file_read_all(path, &len)) != NULL) {
					if (xcindex_scheme_suppresses_target(
					    text, guid, name, container))
						*suppressed = 1;
					free(text);
				}
				if (*suppressed)
					break;
			}
			closedir(rd);
			if (*suppressed)
				break;
		}
		closedir(d);
	}
}

/*
 * The container name a project is referred to by in its own schemes.
 * The fixture writes "container:Sources.xcodeproj"; the project is the
 * bundle, so the directory name of the path (with a .pbxproj path cut
 * down to its bundle) is the comparison.
 */
static void
xcindex_container(const char *project, char *buf, size_t len)
{
	char dir[PATH_MAX];
	const char *slash;

	if (endswith(project, ".pbxproj")) {
		if (xc_dirname(project, dir, sizeof(dir)) != NULL)
			project = dir;
	}

	if ((slash = strrchr(project, '/')) != NULL)
		project = slash + 1;

	snprintf(buf, len, "container:%s", project);
}

/*
 * A scheme name resolves when a file of that name exists (shared, then
 * any user's), or when it is a target-derived scheme: the target of
 * that name exists and no scheme file's runnable suppresses it.
 */
int
xcindex_scheme_resolve(const char *project, const char *scheme,
    int *from_file)
{
	char path[PATH_MAX];

	if (from_file != NULL)
		*from_file = 0;
	if (project == NULL)
		return 0;

	if (project == NULL || scheme == NULL)
		return 0;

	if (scheme_file(project, scheme, path, sizeof(path))) {
		if (from_file != NULL)
			*from_file = 1;
		return 1;
	}

	{
		CFTypeRef root = project_load_pbxproj(project);
		int found = 0;

		if (root != NULL) {
			char guid[512];
			CFTypeRef root_id = NULL, objects, project_obj, tarr;
			int suppressed = 0;

			objects = get_objects_dict(root, &root_id);
			project_obj = pderef(objects, root_id);
			tarr = pget(project_obj, "targets");

			for (CFIndex i = 0; i < pcount(tarr); i++) {
				CFTypeRef tid = pat(tarr, i);
				CFTypeRef tobj = pderef(objects, tid);
				char nb[512];
				const char *tn, *id;

				if (tobj == NULL)
					continue;
				tn = pstr(pget(tobj, "name"), nb, sizeof(nb));
				id = pstr(tid, guid, sizeof(guid));
				if (tn == NULL || strcmp(tn, scheme) != 0)
					continue;

				{
					char container[PATH_MAX];

					xcindex_container(project, container,
					    sizeof(container));
					xcindex_collect_suppressing_text(project,
					    id, tn, container, &suppressed);
				}

				if (!suppressed)
					found = 1;
				break;
			}

			CFRelease(root);
		}

		return found;
	}
}

/*
 * The targets a scheme selects, named by the scheme's runnable.
 *
 * A scheme file's runnable names one buildable -- usually the product
 * the scheme runs.  That reference is looked up in the loaded project:
 * its BlueprintIdentifier is the target's object id, so the target it
 * stands for is resolved by name.  A target-derived scheme selects the
 * one target it is named after.
 */
int
xcindex_scheme_targets(const char *project, const char *scheme,
    char ***names)
{
	char path[PATH_MAX];
	char **targets = NULL;
	int n = 0;

	if (project == NULL) {
		*names = NULL;
		return 0;
	}

	*names = NULL;
	if (project == NULL || scheme == NULL)
		return 0;

	if (!scheme_file(project, scheme, path, sizeof(path))) {
		/* Target-derived: the target is the name. */
		targets = malloc(sizeof(*targets));
		if (targets == NULL)
			return 0;
		targets[0] = strdup(scheme);
		if (targets[0] == NULL) {
			free(targets);
			return 0;
		}
		*names = targets;
		return 1;
	}

	{
		CFTypeRef root = project_load_pbxproj(project);
		char *text;
		size_t len;
		const char *from, *rs, *re;

		if ((text = file_read_all(path, &len)) == NULL) {
			if (root != NULL)
				CFRelease(root);
			return 0;
		}

		from = text;
		while (scheme_runnable_next(text, &from, 0, &rs, &re)) {
			char gbuf[512], nbuf[512], bbuf[512];
			char nb[512];
			const char *g, *bname, *b;
			CFTypeRef root_id = NULL, objects, project_obj, tarr;

			g = xml_attr(rs, re, "BlueprintIdentifier", gbuf,
			    sizeof(gbuf));
			bname = xml_attr(rs, re, "BlueprintName", nbuf,
			    sizeof(nbuf));
			b = xml_attr(rs, re, "BuildableName", bbuf, sizeof(bbuf));

			if (root != NULL) {
				objects = get_objects_dict(root, &root_id);
				if (objects == NULL || root_id == NULL)
					break;
				project_obj = pderef(objects, root_id);
				tarr = pget(project_obj, "targets");

				for (CFIndex i = 0; i < pcount(tarr); i++) {
					CFTypeRef tid = pat(tarr, i);
					CFTypeRef tobj = pderef(objects, tid);
					const char *tn, *id;
					char guid[512];

					if (tobj == NULL)
						continue;
					tn = pstr(pget(tobj, "name"), nb,
					    sizeof(nb));
					id = pstr(tid, guid, sizeof(guid));
					if (tn == NULL)
						continue;

					if ((g != NULL && id != NULL &&
					     strcmp(g, id) == 0) ||
					    (bname != NULL && strcmp(bname, tn) == 0) ||
					    (b != NULL && strcmp(b, tn) == 0)) {
						char **grown =
						    realloc(targets,
						    (size_t)(n + 1) *
						    sizeof(*targets));

						if (grown != NULL) {
							targets = grown;
							targets[n++] = strdup(tn);
						}
						break;
					}
				}
			}
		}

		free(text);
		if (root != NULL)
			CFRelease(root);

		*names = targets;
		return n;
	}
}

/*
 * Every target of the project, in file order, with the object id each
 * is keyed by.  xcindex-test composes an index id from the two: the
 * directory of the project and the target's name and id.
 */
int
xcindex_target_list(const char *project, char ***names, char ***guids)
{
	CFTypeRef root = project_load_pbxproj(project);
	char **nlist = NULL, **glist = NULL;
	int n = 0;

	if (project == NULL) {
		if (guids != NULL)
			*guids = NULL;
		*names = NULL;
		return 0;
	}
	*names = NULL;
	if (guids != NULL)
		*guids = NULL;

	if (root == NULL)
		return 0;

	{
		CFTypeRef root_id = NULL, objects, project_obj, tarr;

		objects = get_objects_dict(root, &root_id);
		project_obj = pderef(objects, root_id);
		tarr = pget(project_obj, "targets");

		for (CFIndex i = 0; i < pcount(tarr); i++) {
			CFTypeRef tid = pat(tarr, i);
			CFTypeRef tobj = pderef(objects, tid);
			char nb[512], gb[512];
			const char *tn, *id;
			char **grown;

			if (tobj == NULL)
				continue;
			tn = pstr(pget(tobj, "name"), nb, sizeof(nb));
			id = pstr(tid, gb, sizeof(gb));
			if (tn == NULL || id == NULL)
				continue;

			grown = realloc(nlist, (size_t)(n + 1) * sizeof(*nlist));
			if (grown != NULL) {
				nlist = grown;
				nlist[n] = strdup(tn);
			}
			grown = realloc(glist, (size_t)(n + 1) * sizeof(*glist));
			if (grown != NULL) {
				glist = grown;
				glist[n] = strdup(id);
			}
			n++;
		}
	}

	CFRelease(root);

	*names = nlist;
	if (guids != NULL)
		*guids = glist;

	return n;
}

/*
 * The scheme list xcindex-test prints.
 *
 * First every scheme file of the project, shared and each user's; then
 * every target-derived scheme (the project's targets) that no scheme
 * file suppresses; and the two are combined without deduplication and
 * sorted under byte order.
 */
int
xcindex_scheme_list(const char *project, char ***names)
{
	strvec list = {0};

	if (project == NULL) {
		*names = NULL;
		return 0;
	}
	char base[PATH_MAX], sdir[PATH_MAX];
	DIR *d;
	CFTypeRef root;
	struct dirent *e;

	*names = NULL;
	if (project == NULL)
		return 0;

	snprintf(base, sizeof(base), "%s", project);

	/* Scheme files: shared, then every user's. */
	snprintf(sdir, sizeof(sdir), "%s/xcshareddata/xcschemes", base);
	if ((d = opendir(sdir)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			char namebuf[256];
			char *dot;

			if (!endswith(e->d_name, ".xcscheme"))
				continue;
			snprintf(namebuf, sizeof(namebuf), "%s", e->d_name);
			if ((dot = strstr(namebuf, ".xcscheme")) != NULL)
				*dot = '\0';
			strvec_push(&list, namebuf);
		}
		closedir(d);
	}

	snprintf(sdir, sizeof(sdir), "%s/xcuserdata", base);
	if ((d = opendir(sdir)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			char udir[PATH_MAX];
			DIR *rd;
			struct dirent *r;

			if (e->d_name[0] == '.')
				continue;

			snprintf(udir, sizeof(udir), "%s/%s/xcschemes", sdir,
			    e->d_name);
			if ((rd = opendir(udir)) == NULL)
				continue;

			while ((r = readdir(rd)) != NULL) {
				char namebuf[256];
				char *dot;

				if (!endswith(r->d_name, ".xcscheme"))
					continue;
				snprintf(namebuf, sizeof(namebuf), "%s",
				    r->d_name);
				if ((dot = strstr(namebuf, ".xcscheme")) != NULL)
					*dot = '\0';
				strvec_push(&list, namebuf);
			}
			closedir(rd);
		}
		closedir(d);
	}

	/* Target-derived schemes do not duplicate a file's name. */
	root = project_load_pbxproj(base);
	if (root != NULL) {
		CFTypeRef root_id = NULL, objects, project_obj, tarr;

		objects = get_objects_dict(root, &root_id);
		project_obj = pderef(objects, root_id);
		tarr = pget(project_obj, "targets");

		for (CFIndex i = 0; i < pcount(tarr); i++) {
			CFTypeRef tid = pat(tarr, i);
			CFTypeRef tobj = pderef(objects, tid);
			char nb[512], gb[512], container[PATH_MAX];
			const char *tn, *id;
			int suppressed_flag = 0;

			if (tobj == NULL)
				continue;
			tn = pstr(pget(tobj, "name"), nb, sizeof(nb));
			id = pstr(tid, gb, sizeof(gb));
			if (tn == NULL || id == NULL)
				continue;

			xcindex_container(base, container, sizeof(container));
			xcindex_collect_suppressing_text(base, id, tn,
			    container, &suppressed_flag);
			if (suppressed_flag)
				continue;

			strvec_push(&list, tn);
		}

		CFRelease(root);
	}

	qsort(list.items, list.count, sizeof(*list.items), xcindex_name_cmp);

	*names = list.items;
	return (int)list.count;
}

/*
 * The source files a target actually compiles, as absolute paths that
 * exist on disk, in build-phase order.  The group tree is walked the
 * same way the build walks it -- a group with a path accumulates it,
 * sourceTree names what the result is measured against, and the walk
 * starts at the directory holding the project bundle -- then each
 * source phase's build file is resolved against what the walk found.
 */

/* Where every file reference of the project lives, keyed by its id. */
struct xpmap {
	char **ids;
	char **paths;
	int count;
	int capacity;
};

static void
xpmap_add(struct xpmap *m, const char *id, const char *path)
{
	char **ids, **paths;
	int cap;

	if (id == NULL)
		return;
	if (m->count == m->capacity) {
		cap = m->capacity ? m->capacity * 2 : 16;
		ids = realloc(m->ids, (size_t)cap * sizeof(*ids));
		paths = realloc(m->paths, (size_t)cap * sizeof(*paths));
		if (ids == NULL || paths == NULL)
			return;
		m->ids = ids;
		m->paths = paths;
		m->capacity = cap;
	}
	m->ids[m->count] = strdup(id);
	m->paths[m->count] = strdup(path);
	m->count++;
}

static const char *
xpmap_get(struct xpmap *m, const char *id)
{
	int i;

	if (id == NULL)
		return NULL;
	for (i = 0; i < m->count; i++)
		if (strcmp(m->ids[i], id) == 0)
			return m->paths[i];
	return NULL;
}

static void
xpmap_free(struct xpmap *m)
{
	int i;

	for (i = 0; i < m->count; i++) {
		free(m->ids[i]);
		free(m->paths[i]);
	}
	free(m->ids);
	free(m->paths);
	m->ids = NULL;
	m->paths = NULL;
	m->count = m->capacity = 0;
}

/*
 * What a file's path is measured against, for sourceTree values that
 * own their own root.  SOURCE_ROOT and the enclosing group both just
 * accumulate; the other roots ignore what has been walked so far.
 */
static void
xwalk_group(CFTypeRef objects, CFTypeRef group, const char *prefix,
    const char *source_root, struct xpmap *map)
{
	CFTypeRef children = pget(group, "children");

	for (CFIndex i = 0; i < pcount(children); i++) {
		CFTypeRef child_id = pat(children, i);
		CFTypeRef child = pderef(objects, child_id);
		char idbuf[512], pathbuf[512], treebuf[64], isabuf[64];
		const char *id, *path, *tree, *isa;
		char full[PATH_MAX];

		if (child == NULL)
			continue;
		id = pstr(child_id, idbuf, sizeof(idbuf));
		isa = pstr(pget(child, "isa"), isabuf, sizeof(isabuf));
		path = pstr(pget(child, "path"), pathbuf, sizeof(pathbuf));
		tree = pstr(pget(child, "sourceTree"), treebuf, sizeof(treebuf));

		if (path == NULL) {
			if (isa != NULL && strstr(isa, "Group") != NULL)
				xwalk_group(objects, child, prefix,
				    source_root, map);
			continue;
		}

		if (path[0] == '/')
			snprintf(full, sizeof(full), "%s", path);
		else if (tree != NULL && strcmp(tree, "SOURCE_ROOT") == 0)
			snprintf(full, sizeof(full), "%s/%s", source_root,
			    path);
		else if (tree != NULL &&
		    (strcmp(tree, "BUILT_PRODUCTS_DIR") == 0 ||
		     strcmp(tree, "SDKROOT") == 0 ||
		     strcmp(tree, "DEVELOPER_DIR") == 0))
			snprintf(full, sizeof(full), "%s", path);
		else
			snprintf(full, sizeof(full), "%s/%s", prefix, path);

		if (isa != NULL && strstr(isa, "Group") != NULL)
			xwalk_group(objects, child, full, source_root, map);
		else if (id != NULL)
			xpmap_add(map, id, full);
	}
}

int
xcindex_target_sources(const char *project, const char *target,
    char ***paths)
{
	struct xpmap map = {0};

	if (project == NULL) {
		*paths = NULL;
		return 0;
	}
	CFTypeRef root, root_id = NULL, objects, project_obj, tarr, tobj = NULL;
	char **list = NULL;
	int n = 0;
	char base[PATH_MAX];

	*paths = NULL;
	if (project == NULL || target == NULL)
		return 0;

	snprintf(base, sizeof(base), "%s", project);
	root = project_load_pbxproj(base);
	if (root == NULL)
		return 0;

	objects = get_objects_dict(root, &root_id);
	if (objects == NULL || root_id == NULL) {
		CFRelease(root);
		return 0;
	}
	project_obj = pderef(objects, root_id);
	tarr = pget(project_obj, "targets");

	for (CFIndex i = 0; i < pcount(tarr); i++) {
		CFTypeRef tid = pat(tarr, i);
		CFTypeRef t = pderef(objects, tid);
		char nb[512];
		const char *tn;

		if (t == NULL)
			continue;
		tn = pstr(pget(t, "name"), nb, sizeof(nb));
		if (tn != NULL && strcmp(tn, target) == 0) {
			tobj = t;
			break;
		}
	}

	if (tobj != NULL) {
		char source_root[PATH_MAX];
		CFTypeRef main_group;

		/* Paths are measured from the directory holding the
		 * project bundle, exactly as the build measures them. */
		if (xc_dirname(base, source_root, sizeof(source_root)) != NULL) {
			main_group = pderef(objects, pget(project_obj,
			    "mainGroup"));
			xwalk_group(objects, main_group, source_root,
			    source_root, &map);

			{
				CFTypeRef phases = pget(tobj, "buildPhases");
				CFIndex p;

				for (p = 0; p < pcount(phases); p++) {
					CFTypeRef phase = pderef(objects,
					    pat(phases, p));
					char isabuf[64];
					const char *isa;
					CFTypeRef files;

					if (phase == NULL)
						continue;
					isa = pstr(pget(phase, "isa"), isabuf,
					    sizeof(isabuf));
					if (isa == NULL ||
					    strcmp(isa, "PBXSourcesBuildPhase") != 0)
						continue;

					files = pget(phase, "files");
					for (CFIndex f = 0; f < pcount(files);
					    f++) {
						CFTypeRef bf = pderef(objects,
						    pat(files, f));
						char refbuf[512];
						const char *ref, *src;
						char **grown;

						if (bf == NULL)
							continue;
						ref = pstr(pget(bf, "fileRef"),
						    refbuf, sizeof(refbuf));
						src = xpmap_get(&map, ref);
						if (src == NULL ||
						    access(src, R_OK) != 0)
							continue;

						grown = realloc(list,
						    (size_t)(n + 1) *
						    sizeof(*list));
						if (grown != NULL) {
							list = grown;
							list[n++] = strdup(src);
						}
					}
				}
			}
		}
	}

	xpmap_free(&map);
	CFRelease(root);

	*paths = list;
	return n;
}

/* ------------------------------------------------------------------ */
/* SDK / toolchain scanning                                           */
/* ------------------------------------------------------------------ */

static void list_dir(const char *base, const char *sub, const char *suffix)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", base, sub);
	DIR *d = opendir(dir);
	if (d == NULL)
		return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (!endswith(e->d_name, suffix))
			continue;
		printf("    %s\n", e->d_name);
	}
	closedir(d);
}

/*
 * -showsdks, in the shape Apple's prints it: one group per platform,
 * each entry the SDK's DisplayName padded out, then the -sdk flag that
 * selects it, which is its CanonicalName.
 *
 * The old implementation listed the flat <dev>/SDKs directory and
 * labelled everything "iOS SDKs" regardless of what it found.
 */

typedef struct {
	char platform[64];
	char display[128];
	char canonical[128];
} sdk_entry;

typedef struct {
	sdk_entry *items;
	size_t count;
	size_t cap;
} sdk_list;

static void collect_sdk(const char *platform, const char *sdkpath, void *ctx)
{
	sdk_list *list = (sdk_list *)ctx;
	char *display, *canonical;
	sdk_entry *e;

	if (list->count == list->cap) {
		size_t cap = list->cap ? list->cap * 2 : 16;
		sdk_entry *items = realloc(list->items, cap * sizeof(*items));

		if (items == NULL)
			return;
		list->items = items;
		list->cap = cap;
	}

	e = &list->items[list->count];
	memset(e, 0, sizeof(*e));

	snprintf(e->platform, sizeof(e->platform), "%s",
		 (platform != NULL && *platform) ? platform : "Unknown");

	display = xt_sdk_setting(sdkpath, "DisplayName");
	canonical = xt_sdk_setting(sdkpath, "CanonicalName");

	if (display != NULL) {
		snprintf(e->display, sizeof(e->display), "%s", display);
		free(display);
	} else {
		/* No settings file: fall back to the directory name. */
		const char *base = strrchr(sdkpath, '/');

		snprintf(e->display, sizeof(e->display), "%s",
			 base != NULL ? base + 1 : sdkpath);
	}

	if (canonical != NULL) {
		snprintf(e->canonical, sizeof(e->canonical), "%s", canonical);
		free(canonical);
	}

	list->count++;
}

static int sdk_entry_cmp(const void *a, const void *b)
{
	const sdk_entry *x = (const sdk_entry *)a;
	const sdk_entry *y = (const sdk_entry *)b;
	int c = strcmp(x->platform, y->platform);

	return (c != 0) ? c : strcmp(x->display, y->display);
}

void project_show_sdks(const char *devpath)
{
	sdk_list list = { NULL, 0, 0 };
	const char *group = NULL;
	size_t i;

	if (!is_dir(devpath)) {
		fprintf(stderr, "xcodebuild: error: developer directory not found at '%s'\n", devpath);
		return;
	}

	xt_foreach_sdk(devpath, collect_sdk, &list);
	if (list.count == 0) {
		printf("No SDKs found in '%s'.\n", devpath);
		return;
	}

	qsort(list.items, list.count, sizeof(*list.items), sdk_entry_cmp);

	for (i = 0; i < list.count; i++) {
		sdk_entry *e = &list.items[i];

		if (group == NULL || strcmp(group, e->platform) != 0) {
			if (group != NULL)
				printf("\n");
			printf("%s SDKs:\n", e->platform);
			group = e->platform;
		}

		if (e->canonical[0] != '\0')
			printf("\t%-30s\t-sdk %s\n", e->display, e->canonical);
		else
			printf("\t%s\n", e->display);
	}

	free(list.items);
}

void project_show_toolchains(const char *devpath)
{
	if (!is_dir(devpath)) {
		fprintf(stderr, "xcodebuild: error: developer directory not found at '%s'\n", devpath);
		return;
	}
	printf("Available toolchains:\n");
	/* Apple's layout first, then the flat one this project used before. */
	list_dir(devpath, "Toolchains", ".xctoolchain");
	list_dir(devpath, "Toolchains", ".toolchain");
}
