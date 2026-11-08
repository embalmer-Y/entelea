/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 平台单调时钟适配（R02）：把 Zephyr k_uptime（native_sim 与板级同源）
 * 包成 R01 实例时钟合同。原始 read 是单计数器读取，ISR 安全——
 * wdt feed 的 ISR 调用面依赖此性质（见 entelea/wdt.h 与监督合同）。
 * 跨重启重新分配 clock_id（沿用 R01 时间合同：旧截止时间不跨重启比较）。
 */
#include <zephyr/kernel.h>
#include <entelea/time.h>

#define ENTELEA_PLATFORM_CLOCK_ID 1U

static entelea_result_t uptime_read_ms(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	*out = k_uptime_get();
	return ENTELEA_OK;
}

entelea_result_t entelea_platform_clock_init(entelea_clock_t *clock)
{
	return entelea_clock_init(clock, uptime_read_ms, NULL,
				  ENTELEA_PLATFORM_CLOCK_ID);
}
