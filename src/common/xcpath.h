/*
 * xcpath.h -- path helpers shared by the tool.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XCODE_TOOLS_XCPATH_H_
#define _XCODE_TOOLS_XCPATH_H_

#include <stddef.h>

/**
 * @func xc_dirname -- the directory holding a path
 * @arg path - the path to take apart
 * @arg buf - storage for the result
 * @arg len - size of @arg buf
 *
 * A path with no '/' in it names something in the current directory, so
 * its directory is ".".  Returning the path itself instead -- which is
 * what a plain "cut at the last slash" gives, having no slash to cut at
 * -- turns a bare "Foo.xcodeproj" into a source root of "Foo.xcodeproj",
 * and every relative path in the project then resolves beneath a file
 * that is not a directory.
 *
 * @return: @arg buf, always NUL-terminated, or NULL if @arg path is
 *          NULL or @arg buf is too small.
 */
const char *xc_dirname(const char *path, char *buf, size_t len);

/**
 * @func xc_abspath -- a path made absolute without requiring it to exist
 * @arg path - the path to resolve
 * @arg buf - storage for the result
 * @arg len - size of @arg buf
 *
 * Unlike realpath(3), the path need not exist, and "." and ".." are resolved
 * textually, so a build directory that has not been created yet still gets a
 * settled name.  A relative path is taken against the current directory; an
 * absolute one is returned cleaned but otherwise unchanged, so the filesystem
 * is never consulted and nothing fails for want of a component.
 *
 * @return: @arg buf, always NUL-terminated, or NULL if @arg path is NULL,
 *          @arg buf is NULL, or the result does not fit.
 */
const char *xc_abspath(const char *path, char *buf, size_t len);

/**
 * @func xc_canonpath -- the one spelling of a path Xcode settles on
 * @arg path - the path to settle
 * @arg buf - storage for the result
 * @arg len - size of @arg buf
 *
 * Symlinks are followed and a leading /private is taken back off, which
 * together turn every way of naming one place into a single string.  It is
 * what Xcode uses wherever a path it was given becomes a path it reports
 * back: a project's SRCROOT, and the name it files the project's arena
 * under.  Asked for one project through a symlink, through the /private
 * form realpath reports, and through the short form, Apple answers with one
 * SRCROOT and one arena, and it is the short form's -- so neither the link's
 * own name nor the /private long form survives into an answer.
 *
 * Unlike xc_abspath(), this does consult the filesystem: the path has to
 * exist to be resolved, and it is the caller that decides what to do when it
 * does not.  A caller with a fallback should use it, since a name is better
 * than none; a caller that needs the settled form should treat NULL as
 * "cannot be settled" rather than "the path is empty".
 *
 * @return: @arg buf, always NUL-terminated, or NULL if @arg path is NULL,
 *          @arg buf is NULL, @arg path does not resolve, or the result does
 *          not fit.
 */
const char *xc_canonpath(const char *path, char *buf, size_t len);

#endif /* _XCODE_TOOLS_XCPATH_H_ */
