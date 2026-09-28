/*
 * devpath.c -- locate our own Developer directory at runtime.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>

#include "devpath.h"

const char *
xt_default_developer_dir(void)
{
	static char devdir[PATH_MAX];
	static int resolved = 0;
	char buf[PATH_MAX], real[PATH_MAX], usrbin[PATH_MAX];
	uint32_t size = sizeof(buf);
	struct stat st;
	char *p;
	int i;

	if (resolved)
		return (devdir[0] != '\0') ? devdir : NULL;

	resolved = 1;
	devdir[0] = '\0';

	if (_NSGetExecutablePath(buf, &size) != 0)
		return NULL;
	if (realpath(buf, real) == NULL)
		return NULL;

	/*
	 * <developer_dir>/usr/bin/<tool> -- strip the tool, then bin, then
	 * usr.  Anything shallower than that is not a Developer layout.
	 */
	for (i = 0; i < 3; i++) {
		if ((p = strrchr(real, '/')) == NULL)
			return NULL;
		*p = '\0';
	}
	if (real[0] == '\0')
		return NULL;

	if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
		return NULL;

	/*
	 * Stripping three components lands on a directory for any executable
	 * sitting three levels deep, and the common case is not a Developer
	 * layout at all: build/release/xcodebuild yields the repository root.
	 * An unconfigured build tree therefore reported itself as DEVELOPER_DIR
	 * and derived every developer key inside it, thirty-odd paths with
	 * nothing behind them.
	 *
	 * Require what every Developer directory has, and what the derived keys
	 * are read from: usr/bin.  CLT has it, Xcode has it, and a staged tree
	 * built the way this repository is built has it, so a real one still
	 * resolves and only the coincidental match is rejected.
	 */
	if (snprintf(usrbin, sizeof(usrbin), "%s/usr/bin", real) >=
	    (int)sizeof(usrbin))
		return NULL;
	if (stat(usrbin, &st) != 0 || !S_ISDIR(st.st_mode))
		return NULL;

	strlcpy(devdir, real, sizeof(devdir));
	return devdir;
}
