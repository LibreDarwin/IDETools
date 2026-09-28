/*
 * xcpath.c -- path helpers shared by the tool.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xcpath.h"

const char *
xc_dirname(const char *path, char *buf, size_t len)
{
	char tmp[PATH_MAX];
	char *slash;

	if (path == NULL || buf == NULL || len == 0)
		return NULL;
	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return NULL;

	slash = strrchr(tmp, '/');
	if (slash == NULL) {
		/* A bare name: what holds it is the current directory. */
		if (len < 2)
			return NULL;
		strcpy(buf, ".");
	} else if (slash == tmp) {
		/* "/Foo" -- the directory is the root itself. */
		if (len < 2)
			return NULL;
		strcpy(buf, "/");
	} else {
		*slash = '\0';
		if (snprintf(buf, len, "%s", tmp) >= (int)len)
			return NULL;
	}

	return buf;
}

const char *
xc_abspath(const char *path, char *buf, size_t len)
{
	char joined[PATH_MAX];
	char out[PATH_MAX];
	const char *p;
	size_t n = 0;

	if (path == NULL || buf == NULL || len == 0)
		return NULL;

	/*
	 * "joined" is always absolute by the time it is walked: a
	 * relative argument is taken against the current directory
	 * first.  So ".." is clamped at the root, and the result can
	 * never be relative even when the input was.
	 */
	if (path[0] == '/') {
		if (snprintf(joined, sizeof(joined), "%s", path) >= (int)sizeof(joined))
			return NULL;
	} else {
		char cwd[PATH_MAX];

		if (getcwd(cwd, sizeof(cwd)) == NULL)
			return NULL;
		if (snprintf(joined, sizeof(joined), "%s/%s", cwd, path) >= (int)sizeof(joined))
			return NULL;
	}

	/*
	 * Settle "." and ".." textually.  Each component is appended
	 * whole, so a name that is itself "." or ".." cannot survive
	 * into the result, and a ".." that would climb above the root is
	 * dropped rather than allowed to escape it.
	 */
	for (p = joined; *p != '\0'; ) {
		const char *end;
		size_t clen;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		end = strchr(p, '/');
		clen = (end != NULL) ? (size_t)(end - p) : strlen(p);

		if (clen == 1 && p[0] == '.') {
			/* no component */
		} else if (clen == 2 && p[0] == '.' && p[1] == '.') {
			/*
			 * Climb one component.  There is nothing to climb
			 * from when the buffer is empty, and a relative
			 * path is already anchored, so that ".." is
			 * dropped in both cases rather than escaping the
			 * root.
			 */
			if (n > 0) {
				while (n > 0 && out[n - 1] != '/')
					n--;
				if (n > 0)
					n--;	/* the separator before it */
			}
		} else {
			if (n > 0 && out[n - 1] != '/')
				out[n++] = '/';
			if (n + clen >= sizeof(out))
				return NULL;
			memcpy(out + n, p, clen);
			n += clen;
		}

		p += clen;
	}

	/*
	 * The leading '/' is written after the components rather than
	 * before them, so the case that is left with an empty buffer --
	 * an absolute path of nothing but ".." -- still comes out as
	 * "/" rather than "".
	 */
	if (n == 0)
		out[n++] = '.';
	out[n] = '\0';

	if (out[0] != '/') {
		if (n + 1 >= sizeof(out))
			return NULL;
		memmove(out + 1, out, n + 1);
		out[0] = '/';
	}

	if (snprintf(buf, len, "%s", out) >= (int)len)
		return NULL;
	return buf;
}
