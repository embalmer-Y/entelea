/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Board images of this experiment have no filesystem and no process clock.
 * Stock Lua references both even when a sandbox removes the file loaders at
 * run time: lauxlib.c keeps luaL_loadfilex (fopen, and through the libc
 * open/read/write/lseek/close) and ltablib.c randomizes the sort pivot with
 * clock(). They link here and fail closed: nothing is ever opened, read,
 * written or timed. The simulator image needs none of them, as its libc is
 * the host's.
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

clock_t
clock(void)
{
	return (clock_t) -1;
}
