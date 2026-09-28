/*
 * xcodebuildLoader.c -- IDETools relaunch trampoline.
 *
 * Copyright (c) 2026 Sunneva N. Mariu
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Apple's loader keeps the tool's entry point in a dylib rather than in the
 * executable, which is what makes the AddressSanitizer relaunch possible: the
 * tool can exec() itself with DYLD_IMAGE_SUFFIX=_asan and dyld will then bind
 * every one of our dylibs to its _asan twin.  See docs/IDETools.md for the
 * reverse-engineered behaviour this reproduces.
 *
 * Apple reaches for DVTFoundation and Xcode3Core here.  Neither exists in this
 * tree, so their roles are filled in locally: environment access is getenv and
 * setenv, the Mach-O scan that finds the runtime is ours, defaults are read
 * through CFPreferences, and the tool dispatch stays in the executable's own
 * main().  The observable contract -- variable names, log messages and decision
 * order -- is preserved.
 *
 * This is C rather than Objective-C to match the rest of the tree, and because
 * MacOSX.Internal.sdk declares @c NSString with none of its methods, so every
 * @c [nsstring someMethod] would be an undeclared selector.  @c NSString and
 * @c CFStringRef are toll-free bridged, so the entry points keep Apple's ABI.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xcodebuildLoader.h"

#define LOADER_ASAN_SUFFIX       "_asan"
#define LOADER_IMAGE_SUFFIX_ENV  "DYLD_IMAGE_SUFFIX"
#define LOADER_INSERT_ENV        "DYLD_INSERT_LIBRARIES"
#define LOADER_RUNTIME_LEAF      "libclang_rt.asan_osx_dynamic.dylib"

#define LOADER_LOG(fmt, ...) \
        fprintf(stderr, "xcodebuild: " fmt "\n", ##__VA_ARGS__)

static int loader_argc;
static char * const *loader_argv;

/*
 * Apple emits these two from its versioned-dylib build step.  They are part of
 * the loader's exported surface, so a clean-room build reproduces the names and
 * keeps the values coherent with the project's own 1.0.0 version.
 */
double xcodebuildLoaderVersionNumber = 1.0;
const unsigned char xcodebuildLoaderVersionString[] =
        "@(#)PROGRAM:xcodebuildLoader  PROJECT:IDETools-1.0.0";

/**
 * @func env_set -- truthiness of an environment variable
 * @arg name - variable name
 * @return true when the variable is present and not empty or "0"
 *
 * Stands in for DVTEnvironmentSnapshotBool, which treats any value other than
 * an explicit false as set.
 */
static Boolean
env_set(const char *name)
{
        const char *v = getenv(name);

        if (v == NULL || *v == '\0')
                return false;
        if (strcmp(v, "0") == 0)
                return false;

        return true;
}

/**
 * @func defaults_bool -- read a boolean user default
 * @arg name - default name
 * @return true when the default is present and true
 *
 * Stands in for [NSUserDefaults boolForKey:], which the loader uses for the
 * same keys it also accepts from the environment.
 */
static Boolean
defaults_bool(const char *name)
{
        CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name,
                                                    kCFStringEncodingUTF8);
        CFTypeRef value;
        Boolean result = false;

        if (key == NULL)
                return false;

        value = CFPreferencesCopyAppValue(key,
                        kCFPreferencesCurrentApplication);
        if (value != NULL) {
                if (CFGetTypeID(value) == CFBooleanGetTypeID())
                        result = CFBooleanGetValue((CFBooleanRef)value) ? true : false;
                CFRelease(value);
        }
        CFRelease(key);

        return result;
}

/**
 * @func basename_of -- final component of a path
 * @arg path - NUL-terminated path
 * @return pointer into @a path at the last '/', or @a path when there is none
 */
static const char *
basename_of(const char *path)
{
        const char *slash = strrchr(path, '/');

        return slash != NULL ? slash + 1 : path;
}

/**
 * @func dirname_into -- copy everything before the last '/'
 * @arg path - NUL-terminated path
 * @param out - receives the parent directory
 * @param size - size of @a out
 * @return 0 on success, -1 when @a path has no parent or @a out is too small
 */
static int
dirname_into(const char *path, char *out, size_t size)
{
        const char *slash = strrchr(path, '/');
        size_t len;

        if (slash == NULL)
                return -1;

        len = (size_t)(slash - path);
        if (len == 0)
                len = 1;                       /* keep the root "/" */
        if (len + 1 > size)
                return -1;

        memcpy(out, path, len);
        out[len] = '\0';

        return 0;
}

/**
 * @func developer_dir_of -- Developer directory containing a tool
 * @arg executablePath - absolute path of the running executable
 * @param out - receives the Developer directory
 * @param size - size of @a out
 * @return 0 on success, -1 when the layout is not recognised
 *
 * Mirrors xt_default_developer_dir() in src/common/devpath.c, which strips
 * three path components from the executable's own location.  The loader is a
 * separate product and cannot call that helper, so the derivation is repeated.
 */
static int
developer_dir_of(const char *executablePath, char *out, size_t size)
{
        char buf[PATH_MAX];

        if (strlen(executablePath) + 1 > sizeof(buf))
                return -1;
        strcpy(buf, executablePath);

        for (int i = 0; i < 3; i++) {
                char parent[PATH_MAX];

                if (dirname_into(buf, parent, sizeof(parent)) != 0)
                        return -1;
                strcpy(buf, parent);
        }

        if (strlen(buf) + 1 > size)
                return -1;
        strcpy(out, buf);

        return 0;
}

/**
 * @func join_into -- append a relative path to a directory
 * @arg dir - directory
 * @arg rel - relative path, may contain '/' separators
 * @param out - receives the joined path
 * @param size - size of @a out
 * @return 0 on success, -1 when the result would not fit
 */
static int
join_into(const char *dir, const char *rel, char *out, size_t size)
{
        int n = snprintf(out, size, "%s/%s", dir, rel);

        if (n < 0 || (size_t)n >= size)
                return -1;

        return 0;
}

/**
 * @func is_regular_file -- whether a path names an existing regular file
 * @arg path - NUL-terminated path
 * @return true when stat() succeeds and the target is not a directory
 */
static Boolean
is_regular_file(const char *path)
{
        struct stat st;

        if (stat(path, &st) != 0)
                return false;

        return S_ISREG(st.st_mode) ? true : false;
}

/**
 * @func is_recognised_tool -- whether we are running a relaunchable IDETools tool
 * @arg executablePath - absolute path of the running executable
 * @arg commandName - expected basename
 * @return true when the path is a Developer usr/bin tool of the expected name
 *
 * Contents/Developer/usr/bin/<tool> is the only layout that carries the dylibs
 * we would ask dyld to re-bind, so anything else is left alone.
 */
static Boolean
is_recognised_tool(const char *executablePath, const char *commandName)
{
        char binDir[PATH_MAX];
        char usrDir[PATH_MAX];
        char devDir[PATH_MAX];
        char want[PATH_MAX];

        if (strcmp(basename_of(executablePath), commandName) != 0)
                return false;

        if (dirname_into(executablePath, binDir, sizeof(binDir)) != 0)
                return false;
        if (dirname_into(binDir, usrDir, sizeof(usrDir)) != 0)
                return false;
        if (dirname_into(usrDir, devDir, sizeof(devDir)) != 0)
                return false;

        if (strcmp(basename_of(binDir), "bin") != 0 ||
            strcmp(basename_of(usrDir), "usr") != 0 ||
            strcmp(basename_of(devDir), "Developer") != 0)
                return false;

        if (developer_dir_of(executablePath, want, sizeof(want)) != 0)
                return false;

        return true;
}

/**
 * @func load_command_references -- dylib names named by a Mach-O image
 * @arg executablePath - absolute path of a Mach-O image
 * @arg leaf - leaf name to look for
 * @param out - receives the matching dylib name as recorded in the image
 * @param size - size of @a out
 * @return 1 when found, 0 when the image names it not, -1 on any read failure
 *
 * Stands in for DVTMachORPathsForExecutable, which we do not have.  Only leaf
 * names are compared: an @rpath entry cannot be resolved without walking dyld's
 * own search order, and the runtime is identified by name.
 */
static int
load_command_references(const char *executablePath, const char *leaf,
                        char *out, size_t size)
{
        FILE *fp = fopen(executablePath, "rb");
        struct mach_header_64 header;
        long pos;                                  /* absolute file offset */
        uint32_t i;
        int found = 0;

        if (fp == NULL)
                return -1;

        if (fread(&header, sizeof(header), 1, fp) != 1)
                goto fail;
        if (header.magic != MH_MAGIC_64)
                goto fail;

        pos = (long)sizeof(header);
        for (i = 0; i < header.ncmds && found == 0; i++) {
                struct load_command cmd;
                long next;

                if (fseek(fp, pos, SEEK_SET) != 0)
                        goto fail;
                if (fread(&cmd, sizeof(cmd), 1, fp) != 1)
                        goto fail;
                if (cmd.cmdsize < sizeof(cmd))
                        goto fail;

                next = pos + (long)cmd.cmdsize;

                if (cmd.cmd == LC_LOAD_DYLIB || cmd.cmd == LC_LOAD_WEAK_DYLIB) {
                        struct dylib_command dylib;
                        char name[PATH_MAX];
                        size_t namelen;

                        if (cmd.cmdsize < sizeof(dylib))
                                goto fail;
                        if (fread(&dylib, sizeof(dylib), 1, fp) != 1)
                                goto fail;

                        /* The name offset is relative to the load_command. */
                        if (dylib.dylib.name.offset < sizeof(dylib) ||
                            (uint32_t)dylib.dylib.name.offset >= cmd.cmdsize)
                                goto fail;
                        if (fseek(fp, pos + (long)dylib.dylib.name.offset,
                                    SEEK_SET) != 0)
                                goto fail;
                        if (fread(name, 1, sizeof(name), fp) != sizeof(name))
                                goto fail;
                        name[sizeof(name) - 1] = '\0';
                        namelen = strnlen(name, sizeof(name));
                        if (namelen > 0 && strcmp(basename_of(name), leaf) == 0) {
                                if (namelen + 1 > size)
                                        goto fail;
                                memcpy(out, name, namelen + 1);
                                found = 1;
                        }
                }

                pos = next;
        }

        fclose(fp);

        return found;

fail:
        fclose(fp);

        return -1;
}

/**
 * @func asan_runtime_path -- the Clang ASan runtime, if we can load it
 * @arg executablePath - absolute path of the running executable
 * @param out - receives an existing path to the runtime
 * @param size - size of @a out
 * @return 0 on success, -1 when the runtime cannot be located
 *
 * The runtime lives in the toolchain rather than being named by the binary, so
 * the load commands are consulted first and the toolchain's clang resource
 * directory second.  A toolchain can carry several clang version directories
 * ("21" and "21.0.0" are both shipped by Xcode 26), and readdir order is
 * arbitrary, so the highest-sorting version that actually holds the runtime
 * wins -- deterministically, rather than depending on directory order.
 */
static int
asan_runtime_path(const char *executablePath, char *out, size_t size)
{
        char developerDir[PATH_MAX];
        char named[PATH_MAX];
        char clangRoot[PATH_MAX];
        char versionDir[PATH_MAX];
        char candidate[PATH_MAX];
        DIR *dir;
        struct dirent *ent;
        char bestVersion[PATH_MAX] = {0};
        char best[PATH_MAX] = {0};

        if (load_command_references(executablePath, LOADER_RUNTIME_LEAF,
                named, sizeof(named)) == 1 && named[0] == '/' &&
            strlen(named) + 1 <= size && is_regular_file(named)) {
                /*
                 * The binary names the runtime outright, so use that path
                 * rather than guessing at a toolchain layout.
                 */
                strcpy(out, named);
                return 0;
        }

        if (developer_dir_of(executablePath, developerDir,
                sizeof(developerDir)) != 0)
                return -1;

        if (join_into(developerDir,
                "Toolchains/XcodeDefault.xctoolchain/usr/lib/clang", clangRoot,
                sizeof(clangRoot)) != 0)
                return -1;

        dir = opendir(clangRoot);
        if (dir == NULL)
                return -1;

        while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.')
                        continue;
                /* "21" and "21.0.0" both exist; the longer one is more specific. */
                if (bestVersion[0] != '\0' &&
                    strcmp(ent->d_name, bestVersion) <= 0)
                        continue;
                if (join_into(clangRoot, ent->d_name, versionDir,
                        sizeof(versionDir)) != 0)
                        continue;
                if (join_into(versionDir, "lib/darwin", versionDir,
                        sizeof(versionDir)) != 0)
                        continue;
                if (join_into(versionDir, LOADER_RUNTIME_LEAF, candidate,
                        sizeof(candidate)) != 0)
                        continue;
                if (!is_regular_file(candidate))
                        continue;
                strcpy(bestVersion, ent->d_name);
                strcpy(best, candidate);
        }
        closedir(dir);

        if (best[0] == '\0' || strlen(best) + 1 > size)
                return -1;
        strcpy(out, best);

        return 0;
}

/**
 * @func XcodeBuildSetInvocation -- record the tool's argument vector
 * @arg argc - argument count, as handed to main()
 * @arg argv - argument vector, as handed to main()
 *
 * A dylib has no portable way to recover the host's argv: @c _NSGetArgc and
 * @c _NSGetArgv still resolve on modern macOS but return garbage, and
 * @c crashreporter.h is absent from MacOSX.Internal.sdk.  The tool therefore
 * hands its own argv over, and the relaunch replays it verbatim.  Until this is
 * called the loader has nothing to replay and declines to relaunch.
 */
void
XcodeBuildSetInvocation(int argc, char * const *argv)
{
        loader_argc = argc;
        loader_argv = argv;
}

/**
 * @func argv_of -- replayable copy of the recorded argument vector
 * @param count - receives the argument count
 * @return a NULL-terminated char * vector, or NULL when argv was never recorded
 */
static char **
argv_of(int *count)
{
        char **copy;
        int i;

        if (loader_argv == NULL || loader_argc <= 0)
                return NULL;

        copy = calloc((size_t)loader_argc + 1, sizeof(char *));
        if (copy == NULL)
                return NULL;

        for (i = 0; i < loader_argc; i++) {
                copy[i] = strdup(loader_argv[i]);
                if (copy[i] == NULL)
                        return NULL;
        }
        *count = loader_argc;

        return copy;
}

void
LoadAddressSanitizerLibrariesIfPresentAndRelaunch(Boolean force,
                                                   CFStringRef commandName)
{
        char executablePath[PATH_MAX];
        char runtime[PATH_MAX];
        char name[256];
        uint32_t size = sizeof(executablePath);
        const char *suffix;
        char **argv;
        int count = 0;

        if (_NSGetExecutablePath(executablePath, &size) != 0)
                return;
        executablePath[PATH_MAX - 1] = '\0';

        if (!CFStringGetCString(commandName, name, sizeof(name),
                    kCFStringEncodingUTF8))
                strcpy(name, "xcodebuild");

        suffix = getenv(LOADER_IMAGE_SUFFIX_ENV);
        if (suffix != NULL && strcmp(suffix, LOADER_ASAN_SUFFIX) == 0) {
                LOADER_LOG("Skipping ASan relaunch because %s is already set to '%s'",
                    LOADER_IMAGE_SUFFIX_ENV, LOADER_ASAN_SUFFIX);
                return;
        }

        if (env_set("DVTNoASanRelaunch") || defaults_bool("DVTNoASanRelaunch")) {
                LOADER_LOG("Skipping ASan relaunch because DVTNoASanRelaunch is set");
                return;
        }

        if (!(force || env_set("FORCE_ASAN_RELAUNCH") ||
              defaults_bool("DVTForceASanRelaunch") ||
              defaults_bool("DVTASanRelaunch"))) {
                LOADER_LOG("Skipping ASan relaunch because it is not the default "
                    "and a force relaunch was not requested");
                return;
        }

        if (defaults_bool("disable-asan-relaunch")) {
                LOADER_LOG("Skipping ASan relaunch because it is disabled by the "
                    "disable-asan-relaunch default");
                return;
        }

        if (!is_recognised_tool(executablePath, name)) {
                LOADER_LOG("Not relaunching under ASan because binary is not "
                    "recognized as supporting doing this: %s", executablePath);
                return;
        }

        if (asan_runtime_path(executablePath, runtime, sizeof(runtime)) != 0) {
                LOADER_LOG("Cannot find path to ASan dylib in binary: %s",
                    executablePath);
                return;
        }

        if (setenv(LOADER_IMAGE_SUFFIX_ENV, LOADER_ASAN_SUFFIX, 1) != 0) {
                LOADER_LOG("Failed to set %s, errno=%d",
                    LOADER_IMAGE_SUFFIX_ENV, errno);
                return;
        }

        if (setenv(LOADER_INSERT_ENV, runtime, 1) != 0) {
                LOADER_LOG("-> Not setting %s because ASan dylib does not exist "
                    "at path: %s", LOADER_INSERT_ENV, runtime);
        } else {
                LOADER_LOG("-> %s=%s", LOADER_INSERT_ENV, runtime);
        }

        argv = argv_of(&count);
        if (argv == NULL) {
                LOADER_LOG("Failed to relaunch %s with %s=%s, errno=%d",
                    executablePath, LOADER_IMAGE_SUFFIX_ENV, LOADER_ASAN_SUFFIX,
                    EINVAL);
                return;
        }

        LOADER_LOG("Relaunching %s with %s=%s", executablePath,
            LOADER_IMAGE_SUFFIX_ENV, LOADER_ASAN_SUFFIX);

        execv(executablePath, argv);

        LOADER_LOG("Failed to relaunch %s with %s=%s, errno=%d", executablePath,
            LOADER_IMAGE_SUFFIX_ENV, LOADER_ASAN_SUFFIX, errno);
}

Boolean
XcodeBuildMain(Boolean allowASanRelaunch, CFStringRef commandName,
               CFStringRef bundleID, CFStringRef logAspectName,
               CFStringRef timelineTraceFormat)
{
        char name[256];

        if (allowASanRelaunch)
                LoadAddressSanitizerLibrariesIfPresentAndRelaunch(false,
                    commandName);

        if (!CFStringGetCString(commandName, name, sizeof(name),
                    kCFStringEncodingUTF8))
                strcpy(name, "xcodebuild");

        LOADER_LOG("%s starts [args: %s]", name, "");
        LOADER_LOG("%s exits [status: %i]", name, 0);

        /*
         * Apple hands off to [Xcode3CommandLineBuildTool
         * sharedCommandLineBuildTool] here.  We have no Xcode3Core, and our
         * tool's own main() already performs the build, so there is nothing to
         * dispatch to; report success and let the caller own the exit status.
         */
        (void)bundleID;
        (void)logAspectName;
        (void)timelineTraceFormat;

        return true;
}
