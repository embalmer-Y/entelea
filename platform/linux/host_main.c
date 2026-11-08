/* SPDX-License-Identifier: Apache-2.0 */
/* R07 Linux 最小宿主：同源合同矩阵主机面验证（D02：最小原生宿主——
 * 公共合同 + 时间适配 + 安全机制（模拟后端）在 Linux 编译运行；
 * 不移植 Zephyr 内核面、不附带完整 Linux 伴侣）。 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/time.h>
#include "clock.h"

static int failures;

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
			failures++;                                            \
		}                                                          \
	} while (0)

static int order_n;
static void sub_a(const entelea_event_t *e, void *u)
{
	(void)e;
	(void)u;
	order_n++;
}
static void sub_b(const entelea_event_t *e, void *u)
{
	(void)e;
	(void)u;
	order_n++;
}

int main(void)
{
	/* ① 实例时钟（Linux 适配）。 */
	entelea_clock_t clk;

	CHECK(entelea_platform_clock_init(&clk) == ENTELEA_OK);
	entelea_instant_t now;

	CHECK(entelea_clock_now(&clk, &now) == ENTELEA_OK);
	CHECK(now.valid);
	CHECK(now.clock_id == 2);
	uint64_t first = now.ms;

	CHECK(entelea_clock_now(&clk, &now) == ENTELEA_OK);
	CHECK(now.ms >= first); /* 单调（CLOCK_MONOTONIC） */

	/* ② 事件总线（静态序/容量）。 */
	CHECK(entelea_evt_bind_clock(&clk) == ENTELEA_OK);
	CHECK(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_a, NULL) ==
	      ENTELEA_OK);
	CHECK(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_b, NULL) ==
	      ENTELEA_OK);
	order_n = 0;
	const entelea_event_t e = {.id = ENTELEA_EVT_BOOT_STEP};

	entelea_evt_publish(&e);
	CHECK(order_n == 2); /* 同步分发 */

	/* ③ 安全机制（模拟后端）：注册/三态/唯一写路径/读回。 */
	CHECK(entelea_safety_bind_clock(&clk) == ENTELEA_OK);
	static const entelea_out_channel_t ch = {
		.uid = "led", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	CHECK(entelea_safety_register_channel(&ch) == ENTELEA_OK);
	CHECK(entelea_safety_commit("led", (entelea_out_value_t){.b = true}) ==
	      ENTELEA_E_STATE); /* SAFE_POWERON 拒 */
	CHECK(entelea_safety_set_link(true) == ENTELEA_OK);
	CHECK(entelea_safety_commit("led", (entelea_out_value_t){.b = true}) ==
	      ENTELEA_OK);
	entelea_out_value_t rb;

	CHECK(entelea_safety_readback("led", &rb) == ENTELEA_OK);
	CHECK(rb.b);
	entelea_safety_force_all_fault();
	entelea_ch_state_t st;

	CHECK(entelea_safety_channel_state("led", &st) == ENTELEA_OK);
	CHECK(st == ENTELEA_ST_SAFE_FAULT);
	CHECK(entelea_safety_commit("led", (entelea_out_value_t){.b = false}) ==
	      ENTELEA_E_STATE); /* 锁存期拒 */

	/* ④ fail reason 留痕钩（弱桩路径——Linux 宿主无存储）。 */
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(1));
	CHECK(1); /* 测试构建返回（非 halt）*/

	printf(failures == 0 ? "HOST SUITE PASS (linux minimal host)\n"
			     : "HOST SUITE FAIL (%d)\n",
	       failures);
	return failures == 0 ? 0 : 1;
}
