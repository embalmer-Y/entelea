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
#include <entelea/storage.h>
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

/* 内核：一切早退路径经外层统一落审计（异常类）。 */
static entelea_result_t commit_impl(const char *uid, entelea_out_value_t v);

/* 压制门控（R04）：本地席位激活期拒绝远程编排写（身份级拒绝，
 * 先于通道态语义；审计留痕）。 */
static entelea_result_t commit_src_gate(entelea_commit_src_t src)
{
	if (src == ENTELEA_SRC_REMOTE &&
	    (entelea_arb_owner_t)atomic_get(&entelea_arb_owner) == ENTELEA_ARB_LOCAL) {
		return ENTELEA_E_STATE;
	}
	return ENTELEA_OK;
}

static entelea_result_t commit_run(int i, entelea_ch_kind_t kind,
				   entelea_out_value_t v)
{
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

	/* 2) slew：总预算锚定最后一次确认写（值=shadow，时刻=last_t）。
	 *    界 = slew × (now - last_t)，无 +1 免跳：同一毫秒内重复提交的
	 *    总变化 ≤ slew（三轮 review 发现 4——每笔 dt+1 使 200 笔同类
	 *    提交可绕过限速）；拆分到界值返回 ENTELEA_E_RANGE（实际落值经
	 *    readback 可观）。成功写刷新锚点（末段临界区），后续提交重新
	 *    从新锚点起算。
	 *    时钟门：本通道带 slew 限 → 首笔（基线建立）与后续提交都要求
	 *    本次读成功——时间不可知不得建立基线或立界。
	 *    溢出：bound 饱和到 u32 极限（u64 ms 时标下实际不可达，仅为
	 *    确定性；等价于本笔不超 u32 域，仍有限）。 */
	if (ch->limits.slew_per_ms > 0) {
		uint64_t now;

		if (!entelea_safety_now_ms_checked(&now)) {
			return ENTELEA_E_STATE; /* 时间不可知：不立界/不建基线 */
		}
		if (s->have_last) {
			if (now < s->last_t) {
				return ENTELEA_E_STATE; /* 时基异常：不伪造界 */
			}
			uint64_t dt = now - s->last_t;
			uint64_t bound64 = (uint64_t)ch->limits.slew_per_ms * dt;
			uint32_t bound = bound64 > 0xFFFFFFFFU ? 0xFFFFFFFFU
							       : (uint32_t)bound64;
			uint32_t last_u = entelea_value_encode(ch->kind, s->shadow);
			uint32_t req_u = entelea_value_encode(ch->kind, v);

			if (req_u > last_u && (uint64_t)(req_u - last_u) > bound) {
				v.u = last_u + bound;
				res = ENTELEA_E_RANGE;
			} else if (req_u < last_u && (uint64_t)(last_u - req_u) > bound) {
				v.u = last_u - bound;
				res = ENTELEA_E_RANGE;
			}
		}
		/* !have_last：该通道自注册后从未有过确认写（真实启动流中
		 * poweron_init 已建立基线，此分支仅无锚点场景）——首笔仅受
		 * min/max/限幅；写入即成为确认写并刷新锚点。 */
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

/* 外层：查表 + 异常审计（含 E_NOTFOUND/安全态/时钟门等全部早退）。 */
entelea_result_t entelea_safety_commit_src(const char *uid,
					   entelea_out_value_t v,
					   entelea_commit_src_t src)
{
	if ((int)src < 0 || src > ENTELEA_SRC_LOCAL) {
		return ENTELEA_E_PARAM;
	}
	if (k_is_in_isr()) {
		return ENTELEA_E_PERM; /* [thread] API */
	}
	if (commit_src_gate(src) != ENTELEA_OK) {
		/* 压制留痕（复用异常审计面）。 */
		int i = entelea_ch_find(uid);

		if (i >= 0) {
			const entelea_storage_audit_entry_t e = {
				.t_ms = entelea_safety_now_ms(),
				.ch_idx = (uint16_t)i,
				.kind = (uint8_t)entelea_ch_table[i].desc->kind,
				.res = ENTELEA_E_STATE,
				.value_u = entelea_value_encode(
					entelea_ch_table[i].desc->kind, v),
			};

			entelea_safety_audit_persist(&e, 0);
		}
		return ENTELEA_E_STATE;
	}
	return commit_impl(uid, v);
}

static entelea_result_t commit_impl(const char *uid, entelea_out_value_t v)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	entelea_ch_kind_t kind = entelea_ch_table[i].desc->kind;
	entelea_result_t res = commit_run(i, kind, v);

	if (res != ENTELEA_OK) {
		const entelea_storage_audit_entry_t e = {
			.t_ms = entelea_safety_now_ms(),
			.ch_idx = (uint16_t)i,
			.kind = (uint8_t)kind,
			.res = res,
			.value_u = entelea_value_encode(kind, v),
		};

		entelea_safety_audit_persist(&e, 0);
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
