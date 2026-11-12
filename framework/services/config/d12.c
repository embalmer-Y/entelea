/* SPDX-License-Identifier: Apache-2.0 */
/* D12 运行时闸实现（合同见 include/entelea/d12.h 头注；阈值=
 * owner 批准 v1 保守方案）。入口经互斥端口串行化，未绑定即拒绝。
 * 单调时间经夹紧（回退沿用最后有效值——窗/冷却只延不缩），冷却在时钟
 * 失明时挂起、首次好读起算。日额按 UTC 日界计，经持久端口保存：放行前
 * 确认当日预留上限有余量，余量耗尽时先写入新预留再放行。 */
#include <string.h>
#include <entelea/time.h>
#include <entelea/d12.h>
#include <entelea/result.h>

#define D12_COOLDOWN_MS 60000U      /* E_BUSY 冷却（429 面） */
#define D12_BOOT_COOLDOWN_MS 60000U /* 异常复位后业务冷却窗 */
#define D12_RATE_WINDOW_MS 60000U
#define D12_RATE_MAX_IN_WINDOW 6U
#define D12_SECS_PER_DAY 86400U
#define D12_REC_VERSION 1U
#define D12_STORM_FAILS 3U          /* 连续失败闩阈值 */

static const entelea_clock_t *d12_clock;
static const entelea_wallclock_t *d12_wall;
static const entelea_lock_t *d12_lock;
static const entelea_d12_store_t *d12_store;
static const uint8_t d12_rec_magic[4] = {'D', '1', '2', 'G'};
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
static bool day_loaded;              /* 已读取持久记录并定下生效日 */
static uint32_t day_utc;             /* 生效 UTC 日（单调，只前进） */
static uint16_t day_reserved;        /* 该日已持久的放行上限 */
static uint16_t day_count;           /* 该日已放行次数 */
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

/* 启动期绑定（运行期只读）；未绑定的端口使依赖它的判定拒绝。 */
entelea_result_t entelea_d12_bind_wallclock(const entelea_wallclock_t *wall)
{
	if (wall == NULL || wall->read_utc_s == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_wall = wall;
	return ENTELEA_OK;
}

entelea_result_t entelea_d12_bind_lock(const entelea_lock_t *lock)
{
	if (lock == NULL || lock->lock == NULL || lock->unlock == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_lock = lock;
	return ENTELEA_OK;
}

entelea_result_t entelea_d12_bind_store(const entelea_d12_store_t *store)
{
	if (store == NULL || store->load == NULL || store->save == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_store = store;
	return ENTELEA_OK;
}

/* 进入临界区：未绑定锁或锁端口拒绝（ISR）时返回非 OK，调用方不得离开。 */
static entelea_result_t d12_enter(void)
{
	if (d12_lock == NULL) {
		return ENTELEA_E_STATE;
	}
	return d12_lock->lock(d12_lock->user);
}

static void d12_leave(void)
{
	d12_lock->unlock(d12_lock->user);
}

void entelea_d12_boot_note(bool abnormal_reset)
{
	uint64_t t;

	if (!abnormal_reset) {
		return;
	}
	if (d12_lock != NULL && d12_enter() == ENTELEA_OK) {
		boot_cooldown_pending = true;
		(void)now_ms(&t);
		d12_leave();
		return;
	}
	/* 启动期尚未绑锁（单线程）：挂起，首次好读时起算 */
	boot_cooldown_pending = true;
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

/* 持久记录（12 字节，小端，KV 保留键）：magic "D12G"、version、flags=0、
 * reserved（当日已持久上限）u16、utc_day（UTC 日序号）u32。 */
static void rec_encode(uint8_t rec[ENTELEA_D12_REC_LEN], uint32_t day,
		       uint16_t reserved)
{
	memcpy(rec, d12_rec_magic, sizeof(d12_rec_magic));
	rec[4] = D12_REC_VERSION;
	rec[5] = 0U;
	rec[6] = (uint8_t)(reserved & 0xFFU);
	rec[7] = (uint8_t)(reserved >> 8);
	rec[8] = (uint8_t)(day & 0xFFU);
	rec[9] = (uint8_t)((day >> 8) & 0xFFU);
	rec[10] = (uint8_t)((day >> 16) & 0xFFU);
	rec[11] = (uint8_t)((day >> 24) & 0xFFU);
}

static bool rec_decode(const uint8_t rec[ENTELEA_D12_REC_LEN], uint32_t *day,
		       uint16_t *reserved)
{
	uint16_t r;

	if (memcmp(rec, d12_rec_magic, sizeof(d12_rec_magic)) != 0 ||
	    rec[4] != D12_REC_VERSION || rec[5] != 0U) {
		return false;
	}
	r = (uint16_t)(rec[6] | (rec[7] << 8));
	if (r > ENTELEA_D12_DAILY_MAX) {
		return false;
	}
	*reserved = r;
	*day = (uint32_t)rec[8] | ((uint32_t)rec[9] << 8) |
	       ((uint32_t)rec[10] << 16) | ((uint32_t)rec[11] << 24);
	return true;
}

/* 首次使用时读取持久记录。无记录 = 当日从零；读失败或记录损坏 = 拒绝，
 * 不以空状态放行。重启后 count 取 reserved：只会偏保守，不会超上限。 */
static entelea_result_t daily_load(uint32_t today)
{
	uint8_t rec[ENTELEA_D12_REC_LEN];
	uint32_t stored_day = 0;
	uint16_t stored_reserved = 0;
	entelea_result_t r;

	if (d12_store == NULL) {
		return ENTELEA_E_STATE;
	}
	r = d12_store->load(d12_store->user, rec);
	if (r == ENTELEA_E_NOTFOUND) {
		day_utc = today;
		day_reserved = 0;
		day_count = 0;
		day_loaded = true;
		return ENTELEA_OK;
	}
	if (r != ENTELEA_OK || !rec_decode(rec, &stored_day, &stored_reserved)) {
		return ENTELEA_E_STATE;
	}
	if (stored_day < today) {
		day_utc = today;
		day_reserved = 0;
		day_count = 0;
	} else {
		/* 同日，或墙钟回退（存储日更晚）：沿用较晚日的上限，不放宽 */
		day_utc = stored_day;
		day_reserved = stored_reserved;
		day_count = stored_reserved;
	}
	day_loaded = true;
	return ENTELEA_OK;
}

/* 日额判定：余量耗尽时按步长预留，先写后放行；写失败即拒绝。 */
static entelea_result_t daily_admit(void)
{
	uint64_t secs = 0;
	uint32_t today;
	uint16_t next;
	uint8_t rec[ENTELEA_D12_REC_LEN];
	entelea_result_t r;

	if (d12_wall == NULL || d12_store == NULL ||
	    d12_wall->read_utc_s(d12_wall->user, &secs) != ENTELEA_OK ||
	    secs / D12_SECS_PER_DAY > UINT32_MAX) {
		return ENTELEA_E_STATE; /* 墙钟未知或日序号超出范围 */
	}
	today = (uint32_t)(secs / D12_SECS_PER_DAY);
	if (!day_loaded) {
		r = daily_load(today);
		if (r != ENTELEA_OK) {
			return r;
		}
	}
	if (today > day_utc) {
		/* 跨日：新日从零开始；回退（today < day_utc）沿用较晚日 */
		day_utc = today;
		day_reserved = 0;
		day_count = 0;
	}
	if (day_count < day_reserved) {
		day_count++;
		return ENTELEA_OK;
	}
	if (day_reserved >= ENTELEA_D12_DAILY_MAX) {
		return ENTELEA_E_BUSY; /* 当日额用尽 */
	}
	next = (uint16_t)(day_reserved + ENTELEA_D12_RESERVE_STEP);
	if (next > ENTELEA_D12_DAILY_MAX) {
		next = (uint16_t)ENTELEA_D12_DAILY_MAX;
	}
	rec_encode(rec, day_utc, next);
	if (d12_store->save(d12_store->user, rec) != ENTELEA_OK) {
		return ENTELEA_E_STATE; /* 写失败：不放行，状态保持 */
	}
	day_reserved = next;
	day_count++;
	return ENTELEA_OK;
}

static entelea_result_t model_allow_locked(void)
{
	uint64_t t;
	entelea_result_t r;

	if (!now_ms(&t)) {
		return ENTELEA_E_STATE; /* 无单调时钟=闭锁（fail-closed） */
	}
	if (storm_latched) {
		blocked_total++;
		return ENTELEA_E_STATE;
	}
	if (in_cooldown(t) || !rate_ok(t)) {
		blocked_total++;
		return ENTELEA_E_BUSY;
	}
	r = daily_admit();
	if (r != ENTELEA_OK) {
		blocked_total++;
		return r;
	}
	window[window_next] = t;
	window_next = (uint8_t)((window_next + 1U) % D12_RATE_MAX_IN_WINDOW);
	if (window_used < D12_RATE_MAX_IN_WINDOW) {
		window_used++;
	}
	calls_total++;
	return ENTELEA_OK;
}

entelea_result_t entelea_d12_model_allow(void)
{
	entelea_result_t r = d12_enter();

	if (r != ENTELEA_OK) {
		return r;
	}
	r = model_allow_locked();
	d12_leave();
	return r;
}

void entelea_d12_model_note(entelea_result_t r)
{
	uint64_t t;

	if (d12_enter() != ENTELEA_OK) {
		return; /* 未绑锁：闸已拒绝，无状态可更新 */
	}
	if (r == ENTELEA_OK) {
		fail_streak = 0;
	} else {
		if (r == ENTELEA_E_BUSY) {
			cooldown_pending = true;
			(void)now_ms(&t);
		}
		if (++fail_streak >= D12_STORM_FAILS) {
			storm_latched = true; /* 不自动恢复（外部 clear） */
			fail_streak = 0;
		}
	}
	d12_leave();
}

bool entelea_d12_storm_latched(void)
{
	bool latched;

	if (d12_clock == NULL || d12_lock == NULL) {
		return true; /* 未绑时钟或锁=保守闭锁 */
	}
	if (d12_enter() != ENTELEA_OK) {
		return true;
	}
	latched = storm_latched;
	d12_leave();
	return latched;
}

void entelea_d12_storm_clear(void)
{
	if (d12_enter() != ENTELEA_OK) {
		return;
	}
	storm_latched = false;
	fail_streak = 0;
	d12_leave();
}

entelea_result_t entelea_d12_wasm_allow(void)
{
	uint64_t t;
	entelea_result_t r = d12_enter();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (!now_ms(&t)) {
		r = ENTELEA_E_STATE;
	} else if (in_boot_cooldown(t)) {
		r = ENTELEA_E_BUSY;
	} else {
		r = ENTELEA_OK;
	}
	d12_leave();
	return r;
}

uint32_t entelea_d12_model_calls(void)
{
	uint32_t n = 0;

	if (d12_enter() == ENTELEA_OK) {
		n = calls_total;
		d12_leave();
	}
	return n;
}

uint32_t entelea_d12_model_blocked(void)
{
	uint32_t n = 0;

	if (d12_enter() == ENTELEA_OK) {
		n = blocked_total;
		d12_leave();
	}
	return n;
}

#ifdef ENTELEA_TEST
void entelea_d12_test_reset(void)
{
	d12_clock = NULL;
	d12_wall = NULL;
	d12_lock = NULL;
	d12_store = NULL;
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
	day_loaded = false;
	day_utc = 0;
	day_reserved = 0;
	day_count = 0;
}
#endif
