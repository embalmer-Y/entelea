/* SPDX-License-Identifier: Apache-2.0 */
/* D12 运行时闸实现（合同见 include/entelea/d12.h 头注；阈值=
 * owner 批准 v1 保守方案）。无分配/无锁（单线程调用面：assist/
 * wasm 装载路径）；时间=绑定时钟同步读，经单调夹紧（回退沿用最后
 * 有效值——窗/冷却只延不缩）。冷却在时钟失明时挂起，首次好读起算。 */
#include <string.h>
#include <entelea/time.h>
#include <entelea/d12.h>
#include <entelea/result.h>

#define D12_COOLDOWN_MS 60000U      /* E_BUSY 冷却（429 面） */
#define D12_BOOT_COOLDOWN_MS 60000U /* 异常复位后业务冷却窗 */
#define D12_RATE_WINDOW_MS 60000U
#define D12_RATE_MAX_IN_WINDOW 6U
#define D12_CALLS_LIFETIME_MAX 200U /* 自启动累计；不等价于每日上限 */
#define D12_STORM_FAILS 3U          /* 连续失败闩阈值 */

static const entelea_clock_t *d12_clock;
static uint64_t last_t;              /* 单调夹紧基准 */
static uint64_t cooldown_start;
static uint64_t boot_cooldown_start;
static bool cooldown_active;
static bool boot_cooldown_active;
static bool cooldown_pending;        /* 上报时时钟失明——待首次好读 */
static bool boot_cooldown_pending;
static uint64_t window[D12_RATE_MAX_IN_WINDOW]; /* 滚动窗（环形） */
static uint8_t window_next;          /* 下一写槽；窗满时即最老槽 */
static uint8_t window_used;
static uint32_t calls_total;
static uint32_t blocked_total;
static uint8_t fail_streak;
static bool storm_latched;           /* 仅 storm_clear 释放 */

static bool now_ms(uint64_t *out)
{
	uint64_t t;

	if (d12_clock == NULL ||
	    d12_clock->read_ms(d12_clock->user, &t) != ENTELEA_OK) {
		return false;
	}
	if (t < last_t) {
		t = last_t; /* 回退：不得经下溢放宽窗/冷却 */
	}
	last_t = t;
	if (boot_cooldown_pending) {
		boot_cooldown_start = t;
		boot_cooldown_active = true;
		boot_cooldown_pending = false;
	}
	if (cooldown_pending) {
		cooldown_start = t;
		cooldown_active = true;
		cooldown_pending = false;
	}
	*out = t;
	return true;
}

bool entelea_d12_reset_guard(bool abnormal, bool history_ok,
			     uint32_t *count)
{
	if (count == NULL) {
		return true;
	}
	if (!abnormal) {
		*count = 0; /* 纯 POR：出风暴窗 */
		return false;
	}
	if (!history_ok) {
		/* 异常复位且跨复位历史不可知（留存面不可用/魔数坏——如
		 * 掉电类复位丢 LP RAM）：无法证明不在风暴中 → 闭锁。 */
		*count = ENTELEA_D12_RESET_STORM_MAX;
		return true;
	}
	if (*count >= ENTELEA_D12_RESET_STORM_MAX) {
		*count = ENTELEA_D12_RESET_STORM_MAX;
		return true;
	}
	(*count)++;
	return *count >= ENTELEA_D12_RESET_STORM_MAX;
}

entelea_result_t entelea_d12_bind_clock(const entelea_clock_t *c)
{
	if (c == NULL || c->read_ms == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_clock = c; /* 风暴闩不随重绑释放 */
	return ENTELEA_OK;
}

void entelea_d12_boot_note(bool abnormal_reset)
{
	uint64_t t;

	if (abnormal_reset) {
		boot_cooldown_pending = true;
		(void)now_ms(&t);
	}
}

static bool in_boot_cooldown(uint64_t t)
{
	return boot_cooldown_active &&
	       t - boot_cooldown_start < D12_BOOT_COOLDOWN_MS;
}

static bool in_cooldown(uint64_t t)
{
	return (cooldown_active && t - cooldown_start < D12_COOLDOWN_MS) ||
	       in_boot_cooldown(t);
}

static bool rate_ok(uint64_t t)
{
	/* 滚动窗：未满可纳；满则看最老槽是否出窗（t 已单调，无下溢）。 */
	return window_used < D12_RATE_MAX_IN_WINDOW ||
	       t - window[window_next] >= D12_RATE_WINDOW_MS;
}

entelea_result_t entelea_d12_model_allow(void)
{
	uint64_t t;

	if (!now_ms(&t)) {
		return ENTELEA_E_STATE; /* 无时钟=闭锁（fail-closed） */
	}
	if (storm_latched) {
		blocked_total++;
		return ENTELEA_E_STATE;
	}
	if (in_cooldown(t)) {
		blocked_total++;
		return ENTELEA_E_BUSY;
	}
	if (calls_total >= D12_CALLS_LIFETIME_MAX || !rate_ok(t)) {
		blocked_total++;
		return ENTELEA_E_BUSY;
	}
	window[window_next] = t;
	window_next = (window_next + 1) % D12_RATE_MAX_IN_WINDOW;
	if (window_used < D12_RATE_MAX_IN_WINDOW) {
		window_used++;
	}
	calls_total++;
	return ENTELEA_OK;
}

void entelea_d12_model_note(entelea_result_t r)
{
	uint64_t t;

	if (r == ENTELEA_OK) {
		fail_streak = 0;
		return;
	}
	if (r == ENTELEA_E_BUSY) {
		cooldown_pending = true;
		(void)now_ms(&t);
	}
	if (++fail_streak >= D12_STORM_FAILS) {
		storm_latched = true; /* 不自动恢复（外部 clear） */
		fail_streak = 0;
	}
}

bool entelea_d12_storm_latched(void)
{
	return d12_clock == NULL || storm_latched; /* 未绑时钟=保守闭锁 */
}

void entelea_d12_storm_clear(void)
{
	storm_latched = false;
	fail_streak = 0;
}

entelea_result_t entelea_d12_wasm_allow(void)
{
	uint64_t t;

	if (!now_ms(&t)) {
		return ENTELEA_E_STATE;
	}
	if (in_boot_cooldown(t)) {
		return ENTELEA_E_BUSY;
	}
	return ENTELEA_OK;
}

uint32_t entelea_d12_model_calls(void)
{
	return calls_total;
}

uint32_t entelea_d12_model_blocked(void)
{
	return blocked_total;
}

#ifdef ENTELEA_TEST
void entelea_d12_test_reset(void)
{
	d12_clock = NULL;
	last_t = 0;
	cooldown_start = 0;
	boot_cooldown_start = 0;
	cooldown_active = false;
	boot_cooldown_active = false;
	cooldown_pending = false;
	boot_cooldown_pending = false;
	memset(window, 0, sizeof(window));
	window_next = 0;
	window_used = 0;
	calls_total = 0;
	blocked_total = 0;
	fail_streak = 0;
	storm_latched = false;
}
#endif
