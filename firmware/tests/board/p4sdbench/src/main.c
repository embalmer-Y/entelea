// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>

#define BLOCK_SIZE 512U
#define BLOCK_COUNT 32U
#define MAX_NAMES 16U

static FATFS volume;
static FIL file;
static uint8_t expected[BLOCK_SIZE] __aligned(64);
static uint8_t actual[BLOCK_SIZE] __aligned(64);

static struct fs_mount_t mount = {
	.type = FS_FATFS,
	.mnt_point = "/SD:",
	.fs_data = &volume,
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};

static void fill_block(unsigned int block)
{
	for (size_t i = 0; i < sizeof(expected); i++) {
		expected[i] = (uint8_t)(i * 31 + block * 17);
	}
}

static int exercise_file(void)
{
	char path[20];
	unsigned int seed = k_cycle_get_32();
	unsigned int count = 0;
	bool created = false;
	bool opened = false;
	FRESULT result = FR_EXIST;
	int ret = -EIO;

	for (unsigned int i = 0; i < MAX_NAMES; i++) {
		snprintk(path, sizeof(path), "SD:/P%07X.TMP", (seed + i) & 0x0fffffff);
		result = f_open(&file, path, FA_WRITE | FA_CREATE_NEW);
		if (result != FR_EXIST) {
			break;
		}
	}
	if (result != FR_OK) {
		printk("P4SD FAIL create fr=%d\n", result);
		return ret;
	}
	created = true;
	opened = true;
	for (unsigned int i = 0; i < BLOCK_COUNT; i++) {
		fill_block(i);
		result = f_write(&file, expected, sizeof(expected), &count);
		if (result != FR_OK || count != sizeof(expected)) {
			printk("P4SD FAIL write block=%u fr=%d bytes=%u\n", i, result, count);
			goto cleanup;
		}
	}
	result = f_sync(&file);
	if (result != FR_OK) {
		printk("P4SD FAIL sync fr=%d\n", result);
		goto cleanup;
	}
	result = f_close(&file);
	opened = false;
	if (result != FR_OK) {
		printk("P4SD FAIL close write fr=%d\n", result);
		goto cleanup;
	}
	result = f_open(&file, path, FA_READ);
	if (result != FR_OK) {
		printk("P4SD FAIL reopen fr=%d\n", result);
		goto cleanup;
	}
	opened = true;
	for (unsigned int i = 0; i < BLOCK_COUNT; i++) {
		fill_block(i);
		result = f_read(&file, actual, sizeof(actual), &count);
		if (result != FR_OK || count != sizeof(actual) ||
		    memcmp(actual, expected, sizeof(actual))) {
			printk("P4SD FAIL read block=%u fr=%d bytes=%u\n", i, result, count);
			goto cleanup;
		}
	}
	result = f_read(&file, actual, 1, &count);
	if (result != FR_OK || count) {
		printk("P4SD FAIL trailing data fr=%d bytes=%u\n", result, count);
		goto cleanup;
	}
	ret = 0;
cleanup:
	if (opened) {
		result = f_close(&file);
		if (result != FR_OK) {
			printk("P4SD FAIL close fr=%d\n", result);
			ret = -EIO;
		}
	}
	if (created) {
		result = f_unlink(path);
		if (result != FR_OK) {
			printk("P4SD FAIL cleanup file=%s fr=%d\n", path, result);
			ret = -EIO;
		}
	}
	return ret;
}

static int geometry(uint32_t *sectors, uint32_t *sector_size)
{
	int ret;

	ret = disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, sectors);
	if (!ret) {
		ret = disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, sector_size);
	}
	if (ret || !*sectors || !*sector_size) {
		printk("P4SD FAIL disk layout rc=%d\n", ret);
		return -EIO;
	}
	return 0;
}

int main(void)
{
	uint32_t sectors = 0;
	uint32_t sector_size = 0;
	uint32_t remount_sectors = 0;
	uint32_t remount_sector_size = 0;
	int ret;
	int cleanup;

	printk("P4SD START no_format=1 exclusive_file=1\n");
	ret = fs_mount(&mount);
	if (ret) {
		printk("P4SD FAIL mount rc=%d\n", ret);
		return 1;
	}
	ret = geometry(&sectors, &sector_size);
	if (!ret) {
		printk("P4SD READY sectors=%u sector_bytes=%u\n", sectors, sector_size);
		ret = exercise_file();
	}
	cleanup = fs_unmount(&mount);
	if (cleanup) {
		printk("P4SD FAIL unmount rc=%d\n", cleanup);
		ret = -EIO;
	}
	if (!ret) {
		ret = fs_mount(&mount);
		if (ret) {
			printk("P4SD FAIL remount rc=%d\n", ret);
		} else {
			ret = geometry(&remount_sectors, &remount_sector_size);
			if (!ret &&
			    (sectors != remount_sectors || sector_size != remount_sector_size)) {
				printk("P4SD FAIL remount geometry changed\n");
				ret = -EIO;
			}
			cleanup = fs_unmount(&mount);
			if (cleanup) {
				printk("P4SD FAIL remount unmount rc=%d\n", cleanup);
				ret = -EIO;
			}
			if (!ret) {
				printk("P4SD REMOUNT_OK\n");
			}
		}
	}
	printk("P4SD %s write_read_bytes=%u\n", ret ? "FAIL" : "PASS", BLOCK_SIZE * BLOCK_COUNT);
	return ret ? 1 : 0;
}
