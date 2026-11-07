// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <string.h>
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/ztest.h>

#define FILE_SIZE (32U * 512U)

#include "diagnostic.h"

enum fault {
	NONE,
	MOUNT_ERROR,
	GEOMETRY_ERROR,
	NAME_COLLISIONS,
	SHORT_WRITE,
	SYNC_ERROR,
	BAD_DATA,
	SHORT_READ,
	TRAILING_DATA,
	UNLINK_ERROR,
	UNMOUNT_ERROR,
	REMOUNT_ERROR,
	REMOUNT_GEOMETRY_ERROR,
};

static enum fault fault;
static uint8_t bytes[FILE_SIZE];
static size_t offset;
static size_t length;
static unsigned int creates;
static unsigned int unlinks;
static unsigned int unmounts;
static unsigned int mounts;
static char created_path[20];

int fs_mount(struct fs_mount_t *mount)
{
	zassert_equal(mount->type, FS_FATFS);
	zassert_true(mount->flags & FS_MOUNT_FLAG_NO_FORMAT);
	zassert_equal(strcmp(mount->mnt_point, "/SD:"), 0);
	mounts++;
	return fault == MOUNT_ERROR || (fault == REMOUNT_ERROR && mounts == 2) ? -ENODEV : 0;
}

int fs_unmount(struct fs_mount_t *mount)
{
	ARG_UNUSED(mount);
	unmounts++;
	return fault == UNMOUNT_ERROR ? -EIO : 0;
}

int disk_access_ioctl(const char *name, uint8_t command, void *data)
{
	zassert_equal(strcmp(name, "SD"), 0);
	if (fault == GEOMETRY_ERROR || (fault == REMOUNT_GEOMETRY_ERROR && mounts == 2)) {
		return -EIO;
	}
	zassert_true(command == DISK_IOCTL_GET_SECTOR_COUNT ||
		     command == DISK_IOCTL_GET_SECTOR_SIZE);
	*(uint32_t *)data = command == DISK_IOCTL_GET_SECTOR_SIZE ? 512 : 65536;
	return 0;
}

FRESULT f_open(FIL *file, const char *path, unsigned char mode)
{
	ARG_UNUSED(file);
	if (mode == (FA_WRITE | FA_CREATE_NEW)) {
		creates++;
		if (fault == NAME_COLLISIONS && creates <= 2) {
			return FR_EXIST;
		}
		zassert_equal(length, 0);
		zassert_true(strlen(path) < sizeof(created_path));
		strcpy(created_path, path);
		offset = 0;
		return FR_OK;
	}
	zassert_equal(mode, FA_READ);
	zassert_equal(strcmp(created_path, path), 0);
	offset = 0;
	return FR_OK;
}

FRESULT f_write(FIL *file, const void *buf, unsigned int size, unsigned int *written)
{
	ARG_UNUSED(file);
	zassert_true(offset + size <= sizeof(bytes));
	memcpy(bytes + offset, buf, size);
	offset += size;
	length = offset;
	*written = fault == SHORT_WRITE ? size - 1 : size;
	return FR_OK;
}

FRESULT f_read(FIL *file, void *buf, unsigned int size, unsigned int *read)
{
	ARG_UNUSED(file);
	size_t remaining = length - offset;
	size_t count = MIN(size, remaining);

	memcpy(buf, bytes + offset, count);
	offset += count;
	*read = count;
	if (fault == BAD_DATA && count) {
		((uint8_t *)buf)[0] ^= 1;
	} else if (fault == SHORT_READ && count) {
		(*read)--;
	} else if (fault == TRAILING_DATA && !count) {
		*read = 1;
	}
	return FR_OK;
}

FRESULT f_sync(FIL *file)
{
	ARG_UNUSED(file);
	return fault == SYNC_ERROR ? FR_DISK_ERR : FR_OK;
}

FRESULT f_close(FIL *file)
{
	ARG_UNUSED(file);
	return FR_OK;
}

FRESULT f_unlink(const char *path)
{
	zassert_equal(strcmp(created_path, path), 0);
	unlinks++;
	return fault == UNLINK_ERROR ? FR_DISK_ERR : FR_OK;
}

static void reset_test(void *fixture)
{
	ARG_UNUSED(fixture);
	fault = NONE;
	offset = 0;
	length = 0;
	creates = 0;
	unlinks = 0;
	unmounts = 0;
	mounts = 0;
	created_path[0] = '\0';
}

ZTEST(sd_diagnostic, test_exclusive_create_sync_exact_read_and_cleanup)
{
	fault = NAME_COLLISIONS;
	zassert_equal(p4sd_diagnostic(), 0);
	zassert_equal(creates, 3);
	zassert_equal(length, FILE_SIZE);
	zassert_equal(unlinks, 1);
	zassert_equal(mounts, 2);
	zassert_equal(unmounts, 2);
}

ZTEST(sd_diagnostic, test_mount_and_geometry_failure_do_not_create_files)
{
	fault = MOUNT_ERROR;
	zassert_not_equal(p4sd_diagnostic(), 0);
	zassert_equal(creates, 0);
	zassert_equal(unmounts, 0);
	fault = GEOMETRY_ERROR;
	zassert_not_equal(p4sd_diagnostic(), 0);
	zassert_equal(creates, 0);
	zassert_equal(unmounts, 1);
}

ZTEST(sd_diagnostic, test_file_and_cleanup_errors_are_not_success)
{
	const enum fault cases[] = {
		SHORT_WRITE,   SYNC_ERROR,    BAD_DATA,
		SHORT_READ,    TRAILING_DATA, UNLINK_ERROR,
		UNMOUNT_ERROR, REMOUNT_ERROR, REMOUNT_GEOMETRY_ERROR,
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		reset_test(NULL);
		fault = cases[i];
		zassert_not_equal(p4sd_diagnostic(), 0);
		zassert_equal(creates, 1);
		zassert_equal(unlinks, 1);
		zassert_equal(unmounts, fault == REMOUNT_GEOMETRY_ERROR ? 2 : 1);
	}
}

ZTEST_SUITE(sd_diagnostic, NULL, NULL, reset_test, NULL, NULL);
