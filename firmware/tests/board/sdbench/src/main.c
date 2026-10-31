/* SPDX-License-Identifier: Apache-2.0 */
/* sdbench（MD1.2a）：SD 卡 bring-up——板载卡槽（Sense 扩展板，SPI：CS21/
 * SCK7/MISO8/MOSI9）挂载 + 写读校验 + 容量上报。
 * 判据（SD* console 行）：
 *   SD1 卡识别 + 挂载 PASS（/SD:）
 *   SD2 顺序写 32KB（4KB×8）→ 回读 memcmp 全等
 *   SD3 断电语义留痕（重挂载幂等——V1 观察项）
 *   SD PASS / SD FAIL */
#include <string.h>
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>

#define MNT "/SD:"

static int write_read_verify(void)
{
	static const char *path = "/SD:/tessera_sdbench.bin";
	static uint8_t wbuf[4096], rbuf[4096]; /* 板级二教训：大缓冲禁上主栈 */

	for (int i = 0; i < (int)sizeof(wbuf); i++) {
		wbuf[i] = (uint8_t)(i * 7 + 3);
	}
	struct fs_file_t f;

	fs_file_t_init(&f);
	int r = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_RDWR);

	if (r != 0) {
		return r;
	}
	for (int blk = 0; blk < 8; blk++) {
		wbuf[0] = (uint8_t)blk; /* 块标识入数据（防全同假阳性） */
		ssize_t n = fs_write(&f, wbuf, sizeof(wbuf));

		if (n != (ssize_t)sizeof(wbuf)) {
			fs_close(&f);
			return -1;
		}
	}
	fs_close(&f);

	r = fs_open(&f, path, FS_O_READ);
	if (r != 0) {
		return r;
	}
	for (int blk = 0; blk < 8; blk++) {
		ssize_t n = fs_read(&f, rbuf, sizeof(rbuf));

		if (n != (ssize_t)sizeof(rbuf)) {
			fs_close(&f);
			return -2;
		}
		wbuf[0] = (uint8_t)blk;
		if (memcmp(rbuf, wbuf, sizeof(wbuf)) != 0) {
			fs_close(&f);
			return -3;
		}
	}
	fs_close(&f);
	fs_unlink(path); /* 清理（下次运行幂等） */
	return 0;
}

int main(void)
{
	printk("SD0 sdbench\n");

	/* 卡初始化（disk_access 底层；DT 的 sdmmc-disk 已注册盘名 "SD"） */
	uint8_t pwr = 0;

	if (disk_access_ioctl("SD", DISK_IOCTL_CTRL_INIT, &pwr) != 0) {
		printk("SD FAIL card init（无卡/接线/供电）\n");
		return 1;
	}
	uint32_t sector_count = 0, sector_size = 0;

	if (disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, &sector_count) != 0 ||
	    disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, &sector_size) != 0) {
		printk("SD FAIL geometry\n");
		return 1;
	}
	printk("SD1 card OK sectors=%u size=%uB total=%uMB\n",
	       sector_count, sector_size,
	       (unsigned)((uint64_t)sector_count * sector_size / 1024U / 1024U));

	static FATFS fat;
	static struct fs_mount_t mp = {
		.type = FS_FATFS,
		.mnt_point = MNT,
		.fs_data = &fat,
	};
	int r = fs_mount(&mp);

	if (r != 0 && r != -EALREADY) {
		printk("SD FAIL mount r=%d\n", r);
		return 1;
	}
	printk("SD2 mounted %s\n", MNT);

	r = write_read_verify();
	if (r != 0) {
		printk("SD FAIL rw r=%d\n", r);
		return 1;
	}
	printk("SD3 write-read-verify 32KB PASS\n");
	printk("SD PASS\n");
	return 0;
}
