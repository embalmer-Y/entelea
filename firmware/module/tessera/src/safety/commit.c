/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 唯一写路径出口（LLD-ts-safety §4）——合同 2 的强制点。
 * 内部流程：查表 → ACTIVE 校验 → 限幅 → slew → 限流 → 末段临界区（irq_lock 复查
 * forced，防 estop ISR 与本线程竞争末笔）→ 审计。
 * 锁收口（DEC-43）：ts_safety_write_lock/unlock 公开给迁移路径与预算检查，
 * ts_safety_commit_locked 供已持锁调用方（ts_power_request 检查-提交原子化）
 * 复用——k_mutex 非递归，持锁方必须走 _locked 变体。锁序：write_lock →
 * pubq（事件发布路径），单向无环。
 */
#include <string.h>
#include <ts/safety.h>
#ifdef CONFIG_TS_STORE_AUDIT
#include <ts/store.h>
#endif
#include "internal.h"

static struct k_mutex commit_lock = Z_MUTEX_INITIALIZER(commit_lock);

void ts_safety_write_lock(void)
{
	__ASSERT(!k_is_in_isr(), "write lock is thread-only");
	(void)k_mutex_lock(&commit_lock, K_FOREVER);
}

void ts_safety_write_unlock(void)
{
	(void)k_mutex_unlock(&commit_lock);
}

/* 审计环形：深度 CONFIG_TS_SAFETY_AUDIT_DEPTH（DEC-27: 64）；溢出覆盖最旧并计数
 * （DR-07）。消费者 = ts-net 遥测合流 + sys:get-audit（M3）。
 * DEC-52①（V2-2）：异常类（res != TS_OK = 安全态拒绝/fail-safe 拒/限幅
 * 截断）下沉 flash 审计环（掉电留存）；正常流不落盘（热路径磨损）。
 * flash 下沉的锁位/节流纪律见下方 F1 修正注记。 */
static ts_audit_entry_t audit_ring[CONFIG_TS_SAFETY_AUDIT_DEPTH];
static uint32_t audit_head, audit_count, audit_dropped;

/* F1 修正（K3 夜间根因，双管齐下）：
 * ① 下沉移出 write_lock——flash 写/擦（4KB 扇区擦最坏数百 ms 起）是锁内
 *   唯一无上界调用，擦除等待/TCP 背压叠加会把安全写锁持有到 WDT 逾期
 *   （真机 noinit 0x20000/1 实证）。pend 槽携带异常条目，公开入口解锁后
 *   同步下沉（同调用内完成，掉电窗口不扩大）。
 * ② 持久化节流——失败提交循环（断链后 APP 每 tick E_STATE）会把异常
 *   下沉放大为 flash 写/擦风暴，cache 关断窗口与 flash 驱动锁竞争反噬
 *   net_wq 喂狗（NET WDT 循环复位；真机擦 APP 对照实证：无失败提交
 *   循环 = 空载恒稳）。首次异常立即下沉，其后风暴 1Hz 采样 + 丢弃计数。
 *   DEC-52① 意图保留（异常掉电留存——首条恒在盘）。 */
#define TS_SAFETY_AUDIT_PERSIST_MIN_MS 1000

static uint32_t audit_persist_throttled;

void ts_safety_audit_persist(const ts_audit_pend_t *pend)
{
#ifdef CONFIG_TS_STORE_AUDIT
	static uint32_t last_persist_ms;

	if (pend != NULL && pend->has) {
		uint32_t now = (uint32_t)ts_time_ms();

		if (last_persist_ms != 0 &&
		    (uint32_t)(now - last_persist_ms) < TS_SAFETY_AUDIT_PERSIST_MIN_MS) {
			audit_persist_throttled++;
			return;
		}
		last_persist_ms = now;
		/* 留痕失败不反噬 commit 路径（观测面降级——失败仅内存环可见） */
		(void)ts_store_audit_append(&pend->e, 0);
	}
#else
	ARG_UNUSED(pend);
#endif
}

uint32_t ts_safety_audit_persist_throttled(void)
{
	return audit_persist_throttled;
}

static void audit_append(int ch_idx, ts_ch_kind_t kind, ts_out_value_t v, ts_res_t res,
			 uint16_t actor, ts_src_t src, ts_audit_pend_t *pend)
{
	uint32_t next = (audit_head + 1) % CONFIG_TS_SAFETY_AUDIT_DEPTH;
	ts_audit_entry_t e = {
		.t_ms = ts_time_ms(),
		.ch_idx = (uint16_t)(ch_idx >= 0 ? ch_idx : 0xFFFF),
		.res = res,
		.actor = actor, /* IR2-08（DEC-48⑥）：调用方归因（0=system；ts-hal 注入 app_id） */
		.kind = (uint8_t)kind,
		.src = (uint8_t)src, /* CG3 · 合同 11：写路径来源维度（旧记录 0=APP） */
		.value_u = ts_value_encode(kind, v),
	};

	if (audit_count == CONFIG_TS_SAFETY_AUDIT_DEPTH) {
		audit_dropped++; /* 覆盖最旧（DR-07） */
	} else {
		audit_count++;
	}
	audit_ring[audit_head] = e;
	audit_head = next;
	if (pend != NULL && res != TS_OK) {
		pend->has = true;
		pend->e = e;
	}
}

static ts_res_t commit_impl(const char *uid, ts_out_value_t v, uint16_t actor, ts_src_t src,
			    ts_audit_pend_t *pend);

ts_res_t ts_safety_commit(const char *uid, ts_out_value_t v)
{
	if (k_is_in_isr()) {
		return TS_E_PERM; /* [thread] API（ISR 禁入，LLD-ts-common §3） */
	}
	ts_audit_pend_t pend = {0};

	ts_safety_write_lock();
	ts_res_t r = commit_impl(uid, v, 0, TS_SRC_APP, &pend);
	ts_safety_write_unlock();
	ts_safety_audit_persist(&pend);
	return r;
}

ts_res_t ts_safety_commit_a(const char *uid, ts_out_value_t v, uint16_t actor)
{
	if (k_is_in_isr()) {
		return TS_E_PERM; /* [thread] API（ISR 禁入，LLD-ts-common §3） */
	}
	ts_audit_pend_t pend = {0};

	ts_safety_write_lock();
	ts_res_t r = commit_impl(uid, v, actor, TS_SRC_APP, &pend);
	ts_safety_write_unlock();
	ts_safety_audit_persist(&pend);
	return r;
}

ts_res_t ts_safety_commit_src(const char *uid, ts_out_value_t v, ts_src_t src)
{
	/* CG3 · 合同 11（LLD §5b）：带来源标记的写路径——远程编排写
	 *（TS_SRC_ZENOH）经此入口（未来 sys 写命令面/远程 hal 写）；压制
	 * 门控在 commit_impl 入口（须持锁后与状态检查同序）。 */
	if (k_is_in_isr()) {
		return TS_E_PERM; /* [thread] API（ISR 禁入，LLD-ts-common §3） */
	}
	ts_audit_pend_t pend = {0};

	ts_safety_write_lock();
	ts_res_t r = commit_impl(uid, v, 0, src, &pend);
	ts_safety_write_unlock();
	ts_safety_audit_persist(&pend);
	return r;
}

ts_res_t ts_safety_commit_locked(const char *uid, ts_out_value_t v, ts_audit_pend_t *pend)
{
	/* 调用方已持 write_lock（DEC-43：预算检查-提交原子化路径）；
	 * pend 由调用方解锁后经 audit_persist 下沉（F1 修正——锁内零 flash） */
	return commit_impl(uid, v, 0, TS_SRC_APP, pend);
}

static ts_res_t commit_impl(const char *uid, ts_out_value_t v, uint16_t actor, ts_src_t src,
			    ts_audit_pend_t *pend)
{
	int i = ts_ch_find(uid);
	if (i < 0) {
		return TS_E_NOTFOUND;
	}
	struct ts_ch_slot *s = &ts_ch_table[i];
	const ts_out_ch_t *ch = s->desc;

	/* 压制门控（CG3 · 合同 11，EN 60204-1 §9.2.4「激活站独占引发权」同型）：
	 * 本地席位激活期拒绝远程编排写（留痕）；APP/LOCAL 来源不受门控
	 *（APP = 编排意图载体——LLD §5b）。置于状态检查前：压制是身份级
	 * 拒绝，优先于通道态语义（审计 res=TS_E_STATE + src 归因可回溯）。 */
	if (src == TS_SRC_ZENOH && (ts_arb_owner_t)atomic_get(&ts_arb_owner) == TS_ARB_LOCAL) {
		audit_append(i, ch->kind, v, TS_E_STATE, actor, src, pend);
		return TS_E_STATE;
	}

	if (s->state != TS_ST_ACTIVE) {
		audit_append(i, ch->kind, v, TS_E_STATE, actor, src, pend); /* 安全态下写入被拒（合同 3） */
		return TS_E_STATE;
	}
	if (atomic_get(&ts_forced) != 0) {
		audit_append(i, ch->kind, v, TS_E_STATE, actor, src, pend);
		return TS_E_STATE; /* fail-safe 锁存期 */
	}

	ts_res_t res = TS_OK;

	/* 1) 限幅：clamp 到 [min,max]（LLD §4-3）。被拦截 = TS_E_RANGE
	 *（err.h：限幅/slew/预算均属“输出被安全层拦截”），拦截事实经审计 res 留痕。 */
	if (ch->kind == TS_CH_PWM) {
		if (v.u < ch->limits.min) {
			v.u = ch->limits.min;
			res = TS_E_RANGE;
		} else if (v.u > ch->limits.max) {
			v.u = ch->limits.max;
			res = TS_E_RANGE;
		}
	}

	/* 2) slew：|Δv| ≤ slew_per_ms × Δt；超率拆分到界值并返回 TS_E_RANGE（LLD §4-4） */
	if (ch->limits.slew_per_ms > 0 && s->have_last) {
		uint64_t dt = ts_time_ms() - s->last_t;
		uint32_t last_u = ts_value_encode(ch->kind, s->shadow);
		uint32_t req_u = ts_value_encode(ch->kind, v);
		uint64_t bound = (uint64_t)ch->limits.slew_per_ms * (dt + 1);

		if (req_u > last_u && (uint64_t)(req_u - last_u) > bound) {
			v.u = last_u + (uint32_t)bound;
			res = TS_E_RANGE;
		} else if (req_u < last_u && (uint64_t)(last_u - req_u) > bound) {
			v.u = last_u - (uint32_t)bound;
			res = TS_E_RANGE;
		}
	}

	/* 3) 限流（TS_CH_POWER）：请求电流超限 → 整笔拒绝（LLD §4-5） */
	if (ch->kind == TS_CH_POWER && v.pwr.en && v.pwr.ma > ch->limits.current_limit_ma) {
		audit_append(i, ch->kind, v, TS_E_RANGE, actor, src, pend);
		return TS_E_RANGE;
	}

	/* 4) 末段临界区：防 estop ISR 与本线程竞争末笔（LLD §4-6）。
	 * IR-02：pre-check 与此处之间 forced 可能被置位——未写回时返回 TS_E_STATE
	 * 并如实审计（否则审计会记录一次从未落驱动的"成功"提交）。 */
	unsigned int key = irq_lock();
	bool written = false;

	if (atomic_get(&ts_forced) == 0) {
		ts_drivers[ch->kind].write(ch, &v);
		s->shadow = v;
		s->last_t = ts_time_ms();
		s->have_last = true;
		written = true;
	}
	irq_unlock(key);
	if (!written) {
		res = TS_E_STATE;
	}

	audit_append(i, ch->kind, v, res, actor, src, pend);
	return res;
}

ts_res_t ts_safety_readback(const char *uid, ts_out_value_t *out)
{
	if (out == NULL) {
		return TS_E_PARAM;
	}
	int i = ts_ch_find(uid);

	if (i < 0) {
		return TS_E_NOTFOUND;
	}
	*out = ts_ch_table[i].shadow;
	return TS_OK;
}

size_t ts_safety_audit_copy(ts_audit_entry_t *out, size_t max)
{
	size_t n = 0;

	for (uint32_t k = 0; k < audit_count && n < max; k++) {
		uint32_t idx = (audit_head + CONFIG_TS_SAFETY_AUDIT_DEPTH - 1 - k) %
			       CONFIG_TS_SAFETY_AUDIT_DEPTH;
		out[n++] = audit_ring[idx];
	}
	return n;
}

uint32_t ts_safety_audit_dropped(void)
{
	return audit_dropped;
}

#ifdef CONFIG_TS_TEST
void ts_safety_audit_test_reset(void)
{
	/* 测试隔离：审计环形随 ts_safety_test_reset 归零（full_scenario 的
	 * 丢弃算术依赖净基线——此前仅因该用例首个执行成立，用例序敏感缺陷） */
	audit_head = 0;
	audit_count = 0;
	audit_dropped = 0;
	memset(audit_ring, 0, sizeof(audit_ring));
}
#endif
