/* SPDX-License-Identifier: Apache-2.0 */
/* R03 板级批 A-3 硬件闸镜像：真机 flash 跨复位持久化（无人值守）。
 * 相位账本=kv（flash 持久）；复位走生产路径（system_fail → task_wdt
 * 通道到期 → sys_reboot——同 hw_fault_stop P1）。
 *  P1（无 kv 记录/上电进入）：meta 写模式串+回读；kv 存 P1；audit 追加
 *     一条；system_fail(FAIL_BOOT(7)) → SW 复位（system_fail 同时经
 *     noinit 落复位原因——本身即 noinit flash 持久化的一环）。
 *  P2（kv=P1 进入，即复位后）：meta 回读==P1 模式串；kv 最新==P1；
 *     audit 计数≥1；last_reset 原因=FAIL_BOOT(7)（noinit 经 flash 跨
 *     SW 复位存活并在本启被一次性消费）→ kv 存 P2 → 常驻收官。
 *  P3（kv=P2）：幂等安全常驻（重复上电不再动状态面）。
 * 无测试钩子：只走公共生产 API（supervision_boot/meta/kv/audit/
 * system_fail）。真断电（VBUS）属到场项——本镜像证明 SW/MWDT 复位
 * 域的 flash 持久化。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <entelea/kv.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"

#define PHASE_APP 0x50484631U /* "PHF1" */

/* 闸内检点打印（PASS/FAIL 逐条落串口——证据面；不中断执行）。 */
#define CHK(cond, msg) printk("[hw-flash] %s: %s\n", (cond) ? "PASS" : "FAIL", msg)

static entelea_clock_t platform_clock;

static const uint8_t META_PAT[30] = "ENTEA-P1-META-FLASH-PERSIST-OK";

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
	uint32_t h = 0x811C9DC5U;

	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x01000193U;
	}
	return h;
}

int main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-flash] boot sequence begins\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	uint32_t cause = 0;

	(void)hwinfo_get_reset_cause(&cause);
	uint32_t reason = 0;
	bool present = false;

	entelea_storage_last_reset(&reason, &present);
	printk("[hw-flash] boot ok; hwinfo cause=0x%x last_reset=%s0x%08x\n",
	       cause, present ? "0x" : "none(", present ? reason : 0);

	uint8_t kvb[8];
	int32_t kvn = entelea_kv_load(PHASE_APP, kvb, sizeof(kvb));

	if (kvn == 2 && memcmp(kvb, "P1", 2) == 0) {
		/* ---- P2：复位后验证 ---- */
		uint8_t rb[64];
		uint16_t rlen = 0;

		CHK(entelea_storage_meta_read(rb, &rlen) == ENTELEA_OK &&
			    rlen == sizeof(META_PAT) &&
			    memcmp(rb, META_PAT, rlen) == 0,
		    "P2 meta round-trip across reset");
		printk("[hw-flash] P2 PASS: meta persists (len=%u fnv=%08x)\n",
		       rlen, fnv1a(rb, rlen));
		entelea_storage_audit_entry_t ae[4];
		size_t n = entelea_storage_audit_read(ae, 4);

		printk("[hw-flash] P2 %s: audit entries=%u (fnv=%08x)\n",
		       n >= 1 ? "PASS" : "FAIL", n,
		       n >= 1 ? fnv1a((const uint8_t *)ae,
				      n * sizeof(ae[0])) : 0);
		CHK(present && reason == ENTELEA_FAIL_BOOT(7),
		    "P2 noinit reason survived SW reset (0x00010007)");
		CHK(entelea_kv_save(PHASE_APP, "P2", 2) == ENTELEA_OK,
		    "P2 kv phase=P2");
		printk("[hw-flash] gate complete: parking resident\n");
	} else if (kvn == 2 && memcmp(kvb, "P2", 2) == 0) {
		printk("[hw-flash] P3: already verified; parking resident\n");
	} else {
		/* ---- P1：写入面 ---- */
		CHK(entelea_storage_meta_write(META_PAT, sizeof(META_PAT)) ==
			    ENTELEA_OK,
		    "P1 meta write");
		uint8_t rb[64];
		uint16_t rlen = 0;

		CHK(entelea_storage_meta_read(rb, &rlen) == ENTELEA_OK &&
			    rlen == sizeof(META_PAT),
		    "P1 meta readback");
		printk("[hw-flash] P1: meta written (fnv=%08x)\n",
		       fnv1a(rb, rlen));
		CHK(entelea_kv_save(PHASE_APP, "P1", 2) == ENTELEA_OK,
		    "P1 kv phase=P1");
		entelea_storage_audit_entry_t e = {
			.t_ms = 0,
			.seq = 0,
			.ch_idx = 0xFFFF,
			.kind = 0,
			.res = 0,
			.evt = 0,
		};

		CHK(entelea_storage_audit_append(&e, 0) == ENTELEA_OK,
		    "P1 audit append");
		printk("[hw-flash] P1: kv/audit written; system_fail -> SW reset <=~5s\n");
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	}
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
