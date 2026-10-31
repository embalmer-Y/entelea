/* SPDX-License-Identifier: Apache-2.0 */
/* 供电槽注册与请求（LLD-ts-power §2）。
 * 开关的物理生效只有 ts_safety_commit 一条路（合同 2/7）；
 * 三安全态：poweron = 描述符声明；linkloss/fault = 关断（本模块生成，
 * 不接受调用方放宽——供电安全侧缺省）。
 * 通道描述符随槽记录**静态存储**（ts-safety 注册表存指针——栈上组装
 * 即悬垂，M3b 实现批自检修复）。 */
#include <string.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/power.h>
#include <ts/safety.h>
#include <ts/store.h>
#include <zephyr/kernel.h>
#include "budget.h"
#include "../safety/internal.h" /* DEC-43：write_lock/commit_locked（检查-提交原子化） */

struct pwr_slot_rec {
	ts_pwr_slot_t spec;
	ts_out_ch_t chan; /* 组装后的通道描述（静态存储，safety 持引用） */
};

static struct pwr_slot_rec recs[CONFIG_TS_POWER_MAX_SLOTS];
static size_t slot_count;

ts_res_t ts_power_register_slot(const ts_pwr_slot_t *s)
{
	if (s == NULL || s->uid == NULL || s->uid[0] == '\0' ||
	    s->current_limit_ma == 0) {
		return TS_E_PARAM;
	}
	if (slot_count >= CONFIG_TS_POWER_MAX_SLOTS) {
		return TS_E_NOMEM;
	}
	for (size_t i = 0; i < slot_count; i++) {
		if (strcmp(recs[i].spec.uid, s->uid) == 0) {
			return TS_E_PARAM; /* 撞名 */
		}
	}
	/* 安全通道（合同 1：三态齐备生成于此） */
	struct pwr_slot_rec *r = &recs[slot_count];

	memset(r, 0, sizeof(*r));
	r->spec = *s;
	r->chan.uid = s->uid;
	r->chan.kind = TS_CH_POWER;
	r->chan.poweron = (ts_out_value_t){
		.pwr = {s->poweron_on, s->poweron_on ? s->current_limit_ma : 0},
	};
	r->chan.linkloss = (ts_out_value_t){.pwr = {false, 0}};
	r->chan.fault = (ts_out_value_t){.pwr = {false, 0}};
	r->chan.limits.current_limit_ma = s->current_limit_ma;
	ts_res_t res = ts_safety_register_channel(&r->chan);

	if (res != TS_OK) {
		return res; /* 不留半注册 */
	}
	slot_count++;
	return TS_OK;
}

size_t ts_power_slot_count(void)
{
	return slot_count;
}

const ts_pwr_slot_t *ts_power_slot_get(size_t idx)
{
	return idx < slot_count ? &recs[idx].spec : NULL;
}

ts_res_t ts_power_request(ts_ctx_t c, uint8_t slot, bool on, uint32_t ma)
{
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_POWER, TS_PERM_OP_SET, slot);

	if (r != TS_OK) {
		return r;
	}
	const ts_pwr_slot_t *s = ts_power_slot_get(slot);

	if (s == NULL) {
		return TS_E_NOTFOUND;
	}
	/* DEC-43 锁收口：预算检查-提交原子化（持锁内走 commit_locked——
	 * k_mutex 非递归）。锁序 write_lock → pubq（预算拒绝事件发布），单向。 */
	ts_safety_write_lock();
	/* 预算检查（LLD §3：超 → 拒绝新请求；既有槽不受影响） */
	uint32_t used = ts_power_used_ma();
	ts_out_value_t rb;

	if (ts_safety_readback(s->uid, &rb) == TS_OK && rb.pwr.en) {
		used -= rb.pwr.ma; /* 本槽重设：先扣除当前占用 */
	}
	if (on && (uint64_t)used + ma > ts_power_budget_ma()) {
		ts_pwr_budget_evt_t pl = {
			.requested_ma = ma,
			.used_ma = used,
			.budget_ma = ts_power_budget_ma(),
		};
		const ts_evt_t evt = {
			.id = TS_EVT_POWER_BUDGET, .t_ms = ts_time_ms(),
			.data = &pl, .len = sizeof(pl),
		};
		ts_evt_publish(&evt);
		ts_safety_write_unlock();
		return TS_E_RANGE;
	}
	/* 唯一写路径（合同 2/7）；限流（current_limit）在保护层兜底 */
	ts_out_value_t v = {.pwr = {.en = on, .ma = on ? ma : 0}};
	ts_audit_pend_t pend = {0};

	r = ts_safety_commit_locked(s->uid, v, &pend);
	ts_safety_write_unlock();
	ts_safety_audit_persist(&pend); /* F1 修正：flash 下沉在锁外 */
	return r;
}

#ifdef CONFIG_TS_TEST
void ts_power_test_reset(void)
{
	memset(recs, 0, sizeof(recs));
	slot_count = 0;
	ts_power_budget_test_reset();
}
#endif
