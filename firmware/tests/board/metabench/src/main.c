/* SPDX-License-Identifier: Apache-2.0 */
/* metabench（MD1.1b）：P1 缺陷最小复现——连续 meta 双副本轮转写 + 暖复位读。
 * 流程（无限循环，每轮 = 一次复位）：boot → 打印 meta_read 结果 + 双副本
 * 原始 16B 十六进制（区分"盘上错"vs"读路径错"）→ meta_write(boot_gen+1)
 * → 打印写后原始态 → 2s 后暖复位。六轮后停。
 * 判读：写 copy0 的轮（第 3/5 次）复位后若读回旧值而盘上原始态正确 =
 * 读路径缺陷；原始态错 = 写路径缺陷。boot_gen 传轮次（跨复位计数）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/store.h>

/* ts-store 后端直读（诊断面）：include 路径 = firmware/module 相对源目录 */
#include "../../../module/tessera/src/store/internal.h"

static void dump_copies(const char *tag)
{
	uint8_t raw[2][16];
	uint32_t stride = ts_store_backend.size(TS_PART_META) / 2;

	for (int c = 0; c < 2; c++) {
		if (ts_store_backend.read(TS_PART_META, c * stride, raw[c], 16) != TS_OK) {
			printk("MB %s copy%d READ-FAIL\n", tag, c);
			return;
		}
	}
	printk("MB %s c0 seq=%02x%02x%02x%02x len=%02x%02x crc=%02x%02x body=%02x\n",
	       tag, raw[0][0], raw[0][1], raw[0][2], raw[0][3],
	       raw[0][4], raw[0][5], raw[0][6], raw[0][7], raw[0][8]);
	printk("MB %s c1 seq=%02x%02x%02x%02x len=%02x%02x crc=%02x%02x body=%02x\n",
	       tag, raw[1][0], raw[1][1], raw[1][2], raw[1][3],
	       raw[1][4], raw[1][5], raw[1][6], raw[1][7], raw[1][8]);
}

int main(void)
{
	printk("MB0 metabench\n");

	ts_appmgr_meta_t meta;
	uint16_t len = 0;
	ts_res_t r = ts_store_meta_read(&meta, &len);

	dump_copies("boot");
	printk("MB1 read=%d slot=%u gen=%u len=%u\n", (int)r,
	       meta.entries[0].active_slot, meta.entries[0].boot_gen, len);

	if (r != TS_OK) {
		/* 首启（双副本擦除态）：从缺省起步 */
		ts_appmgr_meta_default(&meta);
	}
	if (meta.entries[0].boot_gen >= 6) {
		printk("MB DONE 六轮完成：末态 slot=%u gen=%u\n",
		       meta.entries[0].active_slot, meta.entries[0].boot_gen);
		for (;;) {
			k_sleep(K_SECONDS(5));
			printk("MB alive\n");
		}
	}
	meta.entries[0].active_slot = (uint8_t)(meta.entries[0].boot_gen + 1);
	meta.entries[0].rollback_count = 0;
	meta.entries[0].boot_gen = (uint16_t)(meta.entries[0].boot_gen + 1);
	meta.entries[0].app_ver_u32 = 0;
	r = ts_store_meta_write(&meta, sizeof(meta));
	printk("MB2 write gen=%u slot=%u r=%d（预期写 copy%d）\n",
	       meta.entries[0].boot_gen, meta.entries[0].active_slot, (int)r,
	       (meta.entries[0].boot_gen % 2 == 1) ? 0 : 1);
	dump_copies("post");

	k_sleep(K_SECONDS(2));
	printk("MB3 warm reboot\n");
	sys_reboot(SYS_REBOOT_WARM);
	return 0;
}
