/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Board images of this experiment have no filesystem and no process clock.
 * Stock Lua reaches for both from its file loaders (lauxlib.c:
 * luaL_loadfilex), from the io and os libraries that are open for a script
 * (fopen, and through the libc open/read/write/lseek/close, remove, rename,
 * tmpfile and clock) and fail closed: nothing is ever opened, read, written,
 * removed or renamed, and no processor time is ever reported. The simulator
 * image needs none of them, as its libc is the host's.
 *
 * The prototypes are those of the ILP32 ABI, where ssize_t and off_t are as
 * wide as int; Zephyr declares them only with its POSIX and filesystem
 * options, which this image does not enable.
 */
#include <errno.h>
#include <stdarg.h>
#include <time.h>

int
open(const char *path, int oflag, ...)
{
	va_list ap;

	(void) path;
	(void) oflag;
	va_start(ap, oflag);
	va_end(ap);
	errno = ENOENT;
	return -1;
}

int
close(int fd)
{
	(void) fd;
	errno = EBADF;
	return -1;
}

int
read(int fd, void *buf, unsigned int count)
{
	(void) fd;
	(void) buf;
	(void) count;
	errno = EBADF;
	return -1;
}

int
write(int fd, const void *buf, unsigned int count)
{
	(void) fd;
	(void) buf;
	(void) count;
	errno = EBADF;
	return -1;
}

int
lseek(int fd, int offset, int whence)
{
	(void) fd;
	(void) offset;
	(void) whence;
	errno = ESPIPE;
	return -1;
}

int
unlink(const char *path)
{
	(void) path;
	errno = ENOENT;
	return -1;
}

int
rename(const char *oldpath, const char *newpath)
{
	(void) oldpath;
	(void) newpath;
	errno = ENOENT;
	return -1;
}

/* There is no process clock either: os.clock reports that no processor time
 * was spent, and the libc clock() needs nothing beyond this. */
clock_t
clock(void)
{
	return (clock_t) 0;
}
