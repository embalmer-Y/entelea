/* SPDX-License-Identifier: Apache-2.0 */
/* dsdbench（MD1.2f）：D-SD demo 真机载体——ts-fs natives 全链（DEC-47④⑤）。
 * 流程：挂载 SD（FAT，sdbench 同款）→ 建工作目录 → 安装 D-SD 包（机械生成
 * 头文件）→ ts_core_boot 装载 APP → APP 状态机（写/读回校验/白名单外拒绝/
 * 列目录/完成标记，[app1/l*] console 行）→ 宿主侧独立复核：
 *   ① dsd-done.txt 存在（APP 完成信号）
 *   ② dsd.txt 内容 = "DSD-" + 64 数字（独立读出逐字节验证）
 *   ③ /SD:/etc/dsd-deny.txt 不存在（白名单外写被拒的物理证据）
 * 判据（DS* console 行）：DS1 挂载 → DS2 安装 → DS3 ACTIVE → DS4 完成标记
 * → DS5 内容复核 → DS6 拒绝证据 → DS PASS。 */
#include <string.h>
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>

#include "dsd_pkg.h"

#define MNT "/SD:"
#define WORK_DIR MNT "/apps-data"
#define DATA_FILE WORK_DIR "/dsd.txt"
#define DONE_FILE WORK_DIR "/dsd-done.txt"
#define DENY_FILE MNT "/etc/dsd-deny.txt"

static const uint8_t root_pub[32]; /* TEST 语义验签（结构检查） */

static void watcher_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	ts_app_info_t info;
	bool active_seen = false;

	for (int i = 0; i < 150; i++) {
		if (ts_appmgr_get_info(&info) == TS_OK &&
		    info.state == TS_APP_ACTIVE) {
			active_seen = true;
			break;
		}
		k_msleep(200);
	}
	if (!active_seen) {
		printk("DS FAIL no ACTIVE\n");
		return;
	}
	printk("DS3 APP ACTIVE app=%s\n", info.app_id);

	/* ① 完成标记（APP 第 5 步写） */
	bool done = false;

	for (int i = 0; i < 150; i++) {
		struct fs_dirent de;

		if (fs_stat(DONE_FILE, &de) == 0 && de.size >= 2) {
			done = true;
			break;
		}
		k_msleep(200);
	}
	if (!done) {
		printk("DS FAIL no done marker\n");
		return;
	}
	printk("DS4 done marker seen\n");

	/* ② 独立内容复核：dsd.txt = "DSD-" + 64 数字（共 68B） */
	static uint8_t rd[128];
	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, DATA_FILE, FS_O_READ) != 0) {
		printk("DS FAIL open data\n");
		return;
	}
	ssize_t n = fs_read(&f, rd, sizeof(rd));

	fs_close(&f);
	bool ok = (n == 68) && memcmp(rd, "DSD-", 4) == 0;

	for (int i = 4; ok && i < 68; i++) {
		if (rd[i] < '0' || rd[i] > '9') {
			ok = false;
		}
	}
	printk("DS5 data verify n=%d %s\n", (int)n, ok ? "OK" : "MISMATCH");
	if (!ok) {
		printk("DS FAIL content\n");
		return;
	}

	/* ③ 白名单外写拒绝的物理证据：deny 文件必须不存在 */
	struct fs_dirent de2;
	bool absent = fs_stat(DENY_FILE, &de2) != 0;

	printk("DS6 deny-file absent=%d（白名单外写被拒的物理证据）\n",
	       (int)absent);
	printk("%s\n", absent ? "DS PASS（写/读回/白名单拒绝/列目录全链）" :
	       "DS FAIL (deny file exists!)");
	for (;;) {
		k_sleep(K_SECONDS(10));
		printk("DS alive t=%u\n", (unsigned)k_uptime_get_32());
	}
}
K_THREAD_DEFINE(ds_tid, 4096, watcher_thread, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
	printk("DS0 dsdbench（MD1.2f：D-SD demo ts-fs 全链）\n");

	static FATFS fat;
	static struct fs_mount_t mp = {
		.type = FS_FATFS,
		.mnt_point = MNT,
		.fs_data = &fat,
	};
	int r = fs_mount(&mp);

	if (r != 0 && r != -EALREADY) {
		printk("DS FAIL mount r=%d\n", r);
		return 1;
	}
	fs_mkdir(WORK_DIR); /* APP 工作目录（-EEXIST 幂等） */
	printk("DS1 SD mounted%s\n", r == -EALREADY ? " (already)" : "");

	ts_app_info_t info;
	ts_res_t ir = ts_appmgr_install(dsd_pkg, sizeof(dsd_pkg), root_pub, &info);

	printk("DS2 install r=%d state=%d slot=%u\n",
	       (int)ir, (int)info.state, info.active_slot);
	if (ir != TS_OK) {
		printk("DS FAIL install\n");
		return 1;
	}
	ts_core_boot(); /* noreturn：装载 APP → ts-fs natives 状态机 */
	return 0;
}
