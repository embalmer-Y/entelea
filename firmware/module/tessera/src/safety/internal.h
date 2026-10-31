/* SPDX-License-Identifier: Apache-2.0 */
/* ts-safety 内部共享（不对外）：通道槽表/forced/link 原子与编码辅助。 */
#ifndef TS_SAFETY_INTERNAL_H__
#define TS_SAFETY_INTERNAL_H__

#include <string.h>
#include <ts/safety.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/* 每通道写序列环形容量（native_sim 桩，L4 golden 数据源）。来源: 结构性数值 */
#define TS_SIM_REC_PER_CH 32

struct ts_ch_slot {
	const ts_out_ch_t *desc;
	ts_ch_state_t state;
	ts_out_value_t shadow;   /* 影子值：SAFE_* 态 = 安全值；ACTIVE = 最后落值 */
	bool have_last;          /* slew 基线是否已建立 */
	uint64_t last_t;         /* 上次 commit 时刻（slew Δt） */
	struct {
		uint32_t head, count;
		ts_write_rec_t ring[TS_SIM_REC_PER_CH];
	} sim_rec;               /* native_sim 桩驱动记录 */
};

extern struct ts_ch_slot ts_ch_table[CONFIG_TS_SAFETY_MAX_CHANNELS];
extern size_t ts_ch_count;

extern atomic_t ts_forced;      /* estop/fail 锁存标志（clear_fault 释放，IR-01） */
extern atomic_t ts_forced_at;   /* forced 置位时刻（低 32 位，estop 补发事件用） */
extern atomic_t ts_link_up;     /* 全局链路标志（LLD §3） */
extern atomic_t ts_arb_owner;   /* 控制源席位（CG3 · 合同 11；ts_arb_owner_t） */

/** estop 补发宣布位复位（clear_fault 调用；IR-01） */
void ts_safety_estop_announce_reset(void);

/* ---- 锁收口（DEC-43，Q-23 方案 A）------------------------------------------
 * write_lock = commit_lock 的受控暴露：安全态迁移路径（set_link / 单通道
 * force/recover / clear_fault）与供电预算检查-提交须与 commit 互斥。
 * 线程上下文专用（ISR 禁入）；已持锁方调用 commit 须走 commit_locked
 * （k_mutex 非递归）。锁序：write_lock → pubq 锁（事件发布路径），单向。 */
void ts_safety_write_lock(void);
void ts_safety_write_unlock(void);

/* F1 修正（K3 夜间根因）：异常审计的 flash 下沉移出 write_lock——pend 槽
 * 由 commit 族公开入口在解锁后持久化（锁内零 flash 上界）。 */
typedef struct {
	bool has;
	ts_audit_entry_t e;
} ts_audit_pend_t;

void ts_safety_audit_persist(const ts_audit_pend_t *pend);
uint32_t ts_safety_audit_persist_throttled(void);
ts_res_t ts_safety_commit_locked(const char *uid, ts_out_value_t v, ts_audit_pend_t *pend);

/* ts_value_encode 已提升为公共 API（safety.h）——审计/遥测统一口径 */

/* 查找槽下标；未注册返回 -1 */
static inline int ts_ch_find(const char *uid)
{
	if (uid == NULL) {
		return -1;
	}
	for (size_t i = 0; i < ts_ch_count; i++) {
		if (ts_ch_table[i].desc != NULL && ts_ch_table[i].desc->uid != NULL &&
		    strcmp(ts_ch_table[i].desc->uid, uid) == 0) {
			return (int)i;
		}
	}
	return -1;
}

#endif /* TS_SAFETY_INTERNAL_H__ */
