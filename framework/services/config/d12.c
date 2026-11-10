/* SPDX-License-Identifier: Apache-2.0 */
/* D12 运行时闸实现（合同见 include/entelea/d12.h 头注；阈值=
 * owner 批准 v1 保守方案）。无分配/无锁（单线程调用面：assist/
 * wasm 装载路径）；时间=绑定时钟同步读。 */
#include <string.h>
#include <entelea/time.h>
#include <entelea/d12.h>
#include <entelea/result.h>

#define D12_COOLDOWN_MS 60000U      /* E_BUSY 冷却（429 面） */
#define D12_BOOT_COOLDOWN_MS 60000U /* 异常复位后业务冷却窗 */
#define D12_RATE_WINDOW_MS 60000U
#define D12_RATE_MAX_IN_WINDOW 6U
#define D12_CALLS_LIFETIME_MAX 200U /* 自启动累计（无 RTC 的诚实日限） */
#define D12_STORM_FAILS 3U          /* 连续失败闩阈值 */

static const entelea_clock_t *clock;
static uint64_t cooldown_until;      /* E_BUSY 冷却 */
static uint64_t boot_cooldown_until; /* 异常复位冷却 */
static uint64_t window[D12_RATE_MAX_IN_WINDOW]; /* 滚动窗（环形） */
static uint8_t window_next;
static uint32_t calls_total;
static uint32_t blocked_total;
static uint8_t fail_streak;
static bool storm_latched = true; /* 未 bind_clock 前保守闭锁 */

static bool now_ms(uint64_t *out)
{
	return clock != NULL && clock->read_ms(clock->user, out) == ENTELEA_OK;
}

entelea_result_t entelea_d12_bind_clock(const entelea_clock_t *c)
{
	if (c == NULL || c->read_ms == NULL) {
		return ENTELEA_E_PARAM;
	}
	clock = c;
	storm_latched = false;
	return ENTELEA_OK;
}

void entelea_d12_boot_note(bool abnormal_reset)
{
	uint64_t t;

	if (abnormal_reset && now_ms(&t)) {
		boot_cooldown_until = t + D12_BOOT_COOLDOWN_MS;
	}
}

static bool in_cooldown(uint64_t t)
{
	return t < cooldown_until || t < boot_cooldown_until;
}

static bool rate_ok(uint64_t t)
{
	/* 滚动窗：环形槽全在窗内则超限。 */
	for (uint8_t i = 0; i < D12_RATE_MAX_IN_WINDOW; i++) {
		if (window[i] == 0 || t - window[i] >= D12_RATE_WINDOW_MS) {
			return true; /* 有槽出窗=可接纳 */
		}
	}
	return false;
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
	if (r == ENTELEA_E_BUSY && now_ms(&t)) {
		cooldown_until = t + D12_COOLDOWN_MS;
	}
	if (++fail_streak >= D12_STORM_FAILS) {
		storm_latched = true; /* 不自动恢复（外部 clear） */
		fail_streak = 0;
	}
}

bool entelea_d12_storm_latched(void)
{
	return storm_latched;
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
	if (t < boot_cooldown_until) {
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
	clock = NULL;
	cooldown_until = 0;
	boot_cooldown_until = 0;
	memset(window, 0, sizeof(window));
	window_next = 0;
	calls_total = 0;
	blocked_total = 0;
	fail_streak = 0;
	storm_latched = true;
}
#endif
