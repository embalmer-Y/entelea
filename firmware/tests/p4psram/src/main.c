/* SPDX-License-Identifier: Apache-2.0 */
/*
 * psrambench（板级六）：PSRAM 挂接验证（docs/board-psram.md）。
 * 流程：P4P1 SMH 探针（读写一致性 + 外部 RAM 地址域证据）→ P4P2 通道注册
 * + 链路声明（直启面，framework.app 同款）→ P4P3 APP 启动（WAMR 实例堆
 * 此时自 PSRAM 分配——runtime 打印池地址）→ evt 写路径全链 → PASS。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <ts/appmgr.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include "app_wasm.h"

static const ts_out_ch_t ps_ch = {
	.uid = "ps0", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t ps_dev = {.uid = "ps0", .kind = TS_DEV_GPIO_OUT};

static bool ps_gpio(void)
{
	ts_out_value_t v;

	if (ts_safety_readback("ps0", &v) != TS_OK) {
		return false;
	}
	return v.b;
}

static bool ps_wait_gpio(bool want)
{
	for (int i = 0; i < 300; i++) {
		if (ps_gpio() == want) {
			return true;
		}
		k_msleep(10);
	}
	return false;
}

int main(void)
{
	printk("P4P0 psrambench\n");

	/* P4P1: SMH 探针——P4 外部 RAM 映射 0x48000000 起（EXTMEM 域；内部 HP SRAM 为
	 * 0x4ffxxxxx），指针值即位置证据。 */
	uint8_t *probe = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, 256);

	if (probe == NULL) {
		printk("P4P FAIL smh alloc\n");
		return 1;
	}
	for (int i = 0; i < 256; i++) {
		probe[i] = (uint8_t)(0xA5 ^ i);
	}
	bool ok = true;

	for (int i = 0; i < 256; i++) {
		ok = ok && probe[i] == (uint8_t)(0xA5 ^ i);
	}
	printk("P4P1 smh probe: buf=%p %s\n", probe,
	       ok ? "读写一致（PSRAM 存活）" : "读写不一致");
	shared_multi_heap_free(probe);
	if (!ok || ((uintptr_t)probe >> 24) != 0x48U) {
		printk("P4P FAIL smh verify (addr domain)\n");
		return 1;
	}

	/* P4P2: 通道 + 链路（直启面；无 core_boot/linkmon 干扰） */
	if (ts_hal_register_dev(&ps_dev) != TS_OK ||
	    ts_safety_register_channel(&ps_ch) != TS_OK) {
		printk("P4P FAIL channel\n");
		return 1;
	}
	ts_safety_set_link(true);

	/* P4P3: APP 启动——WAMR 实例堆经 runtime 自 PSRAM 分配（console 有
	 * "[appmgr] wamr pool heap: buf=0x48... (psram/smh)" 行 = 位置证据） */
	if (ts_appmgr_app_start(1, app_wasm, app_wasm_len, "gpio:write:0-3") != TS_OK) {
		printk("P4P FAIL app_start\n");
		return 1;
	}
	if (!ps_wait_gpio(true)) {
		printk("P4P FAIL app_init write\n");
		return 1;
	}
	printk("P4P3 app_init -> gpio=1（WAMR 池=PSRAM，全链）\n");
	if (ts_appmgr_app_evt(0) == TS_OK && ps_wait_gpio(false)) {
		printk("P4P3 app_evt(0) -> gpio=0\n");
	} else {
		printk("P4P FAIL app_evt\n");
		return 1;
	}
	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	printk("P4P PASS init_res=%d evt_seen=%u heap=%uKB@PSRAM\n",
	       (int)st.init_res, st.evt_seen, CONFIG_TS_APP_WAMR_HEAP / 1024);
	for (;;) {
		k_msleep(5000);
		printk("P4P alive t=%u\n", (unsigned)k_uptime_get_32());
	}
}
