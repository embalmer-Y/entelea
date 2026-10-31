/* SPDX-License-Identifier: Apache-2.0 */
/* auditbench（V2-2 / DEC-52①）：审计 flash 环掉电留存真机验证。
 *
 * 结构 = bench 惯例（deploybench 模式）：main → ts_core_boot（noreturn；
 * audit_init 步骤扫描恢复 + boot_report 打印历史尾 8 条 = AB3 判读面）
 * → watcher 线程跑两轮逻辑（每轮 = 一次暖复位）：
 *   轮 1（fresh）：注入异常审计（commit 超限 E_RANGE ×3 + estop 沿 ×1）
 *     → AB1 written=4 / AB2 readback（seq 连续）→ 暖复位
 *   轮 2（retained）：boot_report 已打印 4 条（RAM 环复位清空——读数全
 *     部来自 flash 分区）；再注入 2 条 → readback=6 → AB PASS → 停。
 * 轮次经 noinit 跨复位（掉电清零 = fresh 起点，恰好）。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <ts/core.h>
#include <ts/safety.h>
#include <ts/store.h>

static uint32_t round_idx;

static const ts_out_ch_t bench_ch = {
	.uid = "abled",
	.kind = TS_CH_PWM,
	.poweron = {.u = 0},
	.linkloss = {.u = 0},
	.fault = {.u = 0},
	.limits = {.min = 0, .max = 500, .slew_per_ms = 1000},
};

static void round_load(void)
{
	uint32_t rec;
	uint16_t len;
	bool fresh;

	if (ts_store_noinit_get(&rec, &len, &fresh) == TS_OK && !fresh &&
	    len == sizeof(rec)) {
		round_idx = rec;
	} else {
		round_idx = 0;
	}
}

static void round_save(uint32_t v)
{
	ts_store_noinit_put(&v, sizeof(v));
}

static int inject_range(int n)
{
	int written = 0;
	ts_out_value_t v = {.u = 600}; /* > max 500 → E_RANGE 限幅审计 */

	for (int i = 0; i < n; i++) {
		if (ts_safety_commit("abled", v) == TS_E_RANGE) {
			written++;
		}
	}
	return written;
}

static int inject_estop(void)
{
	ts_safety_force_all_fault();
	ts_safety_estop_deferred_publish(); /* 补发路径挂 flash（evt=1） */
	ts_safety_clear_fault();            /* 恢复（下一轮可再注入） */
	return 1;
}

static int readback_check(int expect_min)
{
	ts_store_audit_entry_t e[16];
	size_t n = ts_store_audit_read(e, 16);

	for (size_t i = 1; i < n; i++) {
		if (e[i].seq != e[i - 1].seq + 1) {
			printk("AB FAIL seq-break at %u\n", (unsigned)i);
			return -1;
		}
	}
	printk("AB readback n=%u seq=%u..%u\n", (unsigned)n,
	       n ? (unsigned)e[0].seq : 0, n ? (unsigned)e[n - 1].seq : 0);
	return (n >= (size_t)expect_min) ? (int)n : -1;
}

static void audit_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	k_sleep(K_SECONDS(1)); /* 等 boot 链完成（audit_init 在尾部步骤） */
	printk("AB0 auditbench watch round=%u\n", round_idx);

	if (ts_safety_register_channel(&bench_ch) != TS_OK) {
		printk("AB FAIL ch-register\n");
		return;
	}
	ts_safety_set_link(true); /* 通道转 ACTIVE（否则 commit = E_STATE 拒绝） */
	if (round_idx == 0) {
		int w = inject_range(3) + inject_estop();

		printk("AB1 written=%d\n", w);
		if (w != 4 || readback_check(4) < 0) {
			printk("AB FAIL fresh-round\n");
			return;
		}
		round_save(1);
		printk("AB warm reboot（2s）\n");
		k_sleep(K_SECONDS(2));
		sys_reboot(SYS_REBOOT_WARM);
	}

	int before = readback_check(4);

	if (before < 0) {
		printk("AB FAIL retained-round readback\n");
		return;
	}
	int w = inject_range(1) + inject_estop();

	printk("AB1b written=%d\n", w);
	int total = readback_check(before + 2);

	if (total < 0) {
		printk("AB FAIL retained-round append\n");
		return;
	}
	printk("AB PASS（留存 %d + 新增 %d = %d）\n", before, w, total);
	printk("AB done（停）\n");
	for (;;) {
		k_sleep(K_SECONDS(5));
		printk("AB idle\n");
	}
}
K_THREAD_DEFINE(audit_watch_tid, 4096, audit_watch, NULL, NULL, NULL, 13, 0, 0);

int main(void)
{
	round_load();
	ts_core_boot(); /* noreturn：audit_init（含 boot_report）在尾步 */
	return 0;
}
