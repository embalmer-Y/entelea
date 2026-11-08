/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 唯一写路径出口（迁自旧 safety/commit.c，R02 合同
 * framework/execution/safety/contract.md）。
 * 内部流程：查表 → ACTIVE 校验 → 限幅 → slew → 限流 → 末段临界区
 * （irq_lock 复查 forced，防 estop ISR 与本线程竞争末笔）。
 * 相对旧实现调整：审计环形/异常下沉（R03）与来源标记/仲裁门控（R04）
 * 未迁入——R02 拒绝事实经返回码与状态可观；commit_locked 保留给 R04
 * 预算检查-提交原子化路径。
 */
#include <string.h>
#include <entelea/safety.h>
#include "internal.h"

static struct k_mutex commit_lock = Z_MUTEX_INITIALIZER(commit_lock);

void entelea_safety_write_lock(void)
{
	__ASSERT(!k_is_in_isr(), "write lock is thread-only");
	(void)k_mutex_lock(&commit_lock, K_FOREVER);
}

void entelea_safety_write_unlock(void)
{
	(void)k_mutex_unlock(&commit_lock);
}

static entelea_result_t commit_impl(const char *uid, entelea_out_value_t v);

entelea_result_t entelea_safety_commit(const char *uid, entelea_out_value_t v)
{
	if (k_is_in_isr()) {
		return ENTELEA_E_PERM; /* [thread] API（ISR 禁入） */
	}
	entelea_safety_write_lock();
	entelea_result_t r = commit_impl(uid, v);
	entelea_safety_write_unlock();
	return r;
}

entelea_result_t entelea_safety_commit_locked(const char *uid,
		entelea_out_value_t v, struct entelea_safety_pend *pend)
{
	/* 调用方已持 write_lock（R04 预算检查-提交原子化路径）；
	 * R02 无持久化载荷，pend 仅保持合同形状。 */
	ARG_UNUSED(pend);
	return commit_impl(uid, v);
}

static entelea_result_t commit_impl(const char *uid, entelea_out_value_t v)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	const entelea_out_channel_t *ch = s->desc;

	if (s->state != ENTELEA_ST_ACTIVE) {
		return ENTELEA_E_STATE; /* 安全态下写入被拒 */
	}
	if (atomic_get(&entelea_forced) != 0) {
		return ENTELEA_E_STATE; /* fail-safe 锁存期 */
	}

	entelea_result_t res = ENTELEA_OK;

	/* 1) 限幅：clamp 到 [min,max]；被拦截 = ENTELEA_E_RANGE（限幅/slew/
	 *    预算均属“输出被安全层拦截”）。 */
	if (ch->kind == ENTELEA_CH_PWM) {
		if (v.u < ch->limits.min) {
			v.u = ch->limits.min;
			res = ENTELEA_E_RANGE;
		} else if (v.u > ch->limits.max) {
			v.u = ch->limits.max;
			res = ENTELEA_E_RANGE;
		}
	}

	/* 2) slew：|Δv| ≤ slew_per_ms × Δt；超率拆分到界值并返回
	 *    ENTELEA_E_RANGE（实际落值经 readback 可观）。now 单调护栏使
	 *    now < last_t 实际不可达；带式保险显式拒绝，绝不放宽界
	 *    （缺陷 5：下溢会得到巨大 dt = 无限制）。 */
	if (ch->limits.slew_per_ms > 0 && s->have_last) {
		uint64_t now = entelea_safety_now_ms();

		if (now < s->last_t) {
			return ENTELEA_E_STATE; /* 时基异常：不伪造 slew 界 */
		}
		uint64_t dt = now - s->last_t;
		uint32_t last_u = entelea_value_encode(ch->kind, s->shadow);
		uint32_t req_u = entelea_value_encode(ch->kind, v);
		uint64_t bound = (uint64_t)ch->limits.slew_per_ms * (dt + 1);

		if (req_u > last_u && (uint64_t)(req_u - last_u) > bound) {
			v.u = last_u + (uint32_t)bound;
			res = ENTELEA_E_RANGE;
		} else if (req_u < last_u && (uint64_t)(last_u - req_u) > bound) {
			v.u = last_u - (uint32_t)bound;
			res = ENTELEA_E_RANGE;
		}
	}

	/* 3) 限流（POWER）：请求电流超限 → 整笔拒绝。 */
	if (ch->kind == ENTELEA_CH_POWER && v.pwr.en &&
	    v.pwr.ma > ch->limits.current_limit_ma) {
		return ENTELEA_E_RANGE;
	}

	/* 4) 末段临界区：防 estop ISR 与本线程竞争末笔。pre-check 与此处
	 *    之间 forced 可能被置位——未写回时返回 ENTELEA_E_STATE；
	 *    后端写失败返回 ENTELEA_E_IO（缺陷 2：后端失败绝不更新影子
	 *    或报告成功）。两种情况都不保留 slew 基线。 */
	unsigned int key = irq_lock();
	bool written = false;

	if (atomic_get(&entelea_forced) == 0) {
		if (entelea_drivers[ch->kind].write(ch, &v) == 0) {
			s->shadow = v;
			s->last_t = entelea_safety_now_ms();
			s->have_last = true;
			written = true;
		}
	}
	irq_unlock(key);
	if (!written) {
		entelea_safety_count_write_failure();
		res = (atomic_get(&entelea_forced) != 0) ? ENTELEA_E_STATE
							 : ENTELEA_E_IO;
	}
	return res;
}

entelea_result_t entelea_safety_readback(const char *uid, entelea_out_value_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	*out = entelea_ch_table[i].shadow;
	return ENTELEA_OK;
}
