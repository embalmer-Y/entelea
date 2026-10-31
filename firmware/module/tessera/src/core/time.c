/* SPDX-License-Identifier: Apache-2.0 */
/* 单调时间服务（LLD-ts-core §3）：合同 9 唯一时间源；测试构建可注入虚拟时钟。 */
#include <ts/core.h>
#include <zephyr/kernel.h>

static uint64_t now_default(void)
{
	return (uint64_t)k_uptime_get();
}

#ifdef CONFIG_TS_TEST
static const ts_time_source_t *bound_src;

void ts_time_test_bind(const ts_time_source_t *src)
{
	bound_src = src;
}

uint64_t ts_time_ms(void)
{
	if (bound_src != NULL) {
		return bound_src->now_ms();
	}
	return now_default();
}
#else
uint64_t ts_time_ms(void)
{
	return now_default();
}
#endif

/* ---- 墙钟（数据字段专用，DR-08；合同 9 控制路径禁用）--------------------- */

static uint64_t wall_epoch_base; /* set 时刻的墙钟值 */
static uint64_t wall_mono_base;  /* set 时刻的单调值 */

void ts_time_wall_set(uint64_t epoch_ms)
{
	wall_mono_base = ts_time_ms();
	wall_epoch_base = epoch_ms;
}

uint64_t ts_time_wall_ms(void)
{
	if (wall_epoch_base == 0 && wall_mono_base == 0) {
		return 0; /* 未设置（合法数据态：调用方自行判 0） */
	}
	return wall_epoch_base + (ts_time_ms() - wall_mono_base);
}
