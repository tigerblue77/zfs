// SPDX-License-Identifier: CDDL-1.0
/*
 * LD_PRELOAD shim for sandboxes without /dev/zfs.
 *
 * libzpool's pool_active() opens /dev/zfs and asks the kernel whether a pool
 * of that name is imported.  Without the device the open fails, and every
 * lookup that does not pass can_be_active (zhack on a writable pool, for one)
 * reports "no pools found".  This maps /dev/zfs to /dev/null: the ioctl then
 * fails and the pool is, correctly, not active.  Nothing else is changed.
 *
 *   gcc -shared -fPIC -o devzfs_shim.so devzfs_shim.c -ldl
 *   LD_PRELOAD=$PWD/devzfs_shim.so ./zhack -d dir raidz_epochs ...
 */
#define	_GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

static const char *
map(const char *path)
{
	return (path != NULL && strcmp(path, "/dev/zfs") == 0 ?
	    "/dev/null" : path);
}

#define	SHIM(name, ...)							\
	static int (*real)(const char *, int, ...);			\
	va_list ap;							\
	mode_t mode = 0;						\
	if (real == NULL)						\
		real = dlsym(RTLD_NEXT, name);				\
	va_start(ap, flags);						\
	if (flags & (O_CREAT | O_TMPFILE))				\
		mode = va_arg(ap, mode_t);				\
	va_end(ap);							\
	return (real(map(path), flags, mode));

int
open(const char *path, int flags, ...)
{
	SHIM("open")
}

int
open64(const char *path, int flags, ...)
{
	SHIM("open64")
}
