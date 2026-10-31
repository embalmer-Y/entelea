/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 通道注册表 + 三安全态状态机（LLD-ts-safety §2/§3）。
 * 合同 1：无三安全态声明不予注册；合同 10：安全参数运行时只读。
 */
#include <string.h>
#include <ts/safety.h>
#include "internal.h"

struct ts_ch_slot ts_ch_table[CONFIG_TS_SAFETY_MAX_CHANNELS]; /* 容量 32（DEC-27） */
size_t ts_ch_count;

atomic_t ts_link_up = ATOMIC_INIT(0); /* 初值 down：boot 后链路未确立 */
atomic_t ts_forced = ATOMIC_INIT(0);
atomic_t ts_forced_at = ATOMIC_INIT(0);
atomic_t ts_arb_owner = ATOMIC_INIT(TS_ARB_REMOTE); /* CG3 · 合同 11：远程编排缺省（本地席位空缺 = 现行为零变化） */

static bool value_within_limits(const ts_out_ch_t *ch, ts_out_value_t v)
{
	switch (ch->kind) {
	case TS_CH_GPIO:
		return true;
	case TS_CH_PWM:
		return v.u >= ch->limits.min && v.u <= ch->limits.max;
	case TS_CH_POWER:
		if (!v.pwr.en) {
			return true;
		}
		return v.pwr.ma <= ch->limits.current_limit_ma;
	default:
		return false;
	}
}

ts_res_t ts_safety_register_channel(const ts_out_ch_t *ch)
{
	if (ch == NULL || ch->uid == NULL || ch->uid[0] == '\0' ||
	    ch->kind >= TS_CH_KIND_COUNT) {
		return TS_E_PARAM;
	}
	if (ts_ch_find(ch->uid) >= 0) {
		return TS_E_PARAM; /* uid 撞名 */
	}
	if (ts_ch_count >= CONFIG_TS_SAFETY_MAX_CHANNELS) {
		return TS_E_NOMEM;
	}
	/* 三安全态缺一拒绝；且安全态值本身必须落在 limits 内（LLD §2 注册校验） */
	if (!value_within_limits(ch, ch->poweron) ||
	    !value_within_limits(ch, ch->linkloss) ||
	    !value_within_limits(ch, ch->fault)) {
		return TS_E_PARAM;
	}

	struct ts_ch_slot *slot = &ts_ch_table[ts_ch_count++];

	slot->desc = ch;
	slot->state = TS_ST_SAFE_POWERON;
	slot->shadow = ch->poweron;
	slot->have_last = false;
	slot->last_t = 0;
	slot->sim_rec.head = 0;
	slot->sim_rec.count = 0;
	return TS_OK;
}

static void set_state(size_t i, ts_ch_state_t next)
{
	struct ts_ch_slot *s = &ts_ch_table[i];

	if (s->state == next) {
		return;
	}
	s->state = next;
	const ts_safe_state_evt_t payload = {
		.uid = s->desc->uid,
		.new_state = next,
	};
	const ts_evt_t evt = {
		.id = TS_EVT_SAFE_STATE_CHANGED,
		.t_ms = ts_time_ms(),
		.data = &payload,
		.len = sizeof(payload),
	};
	ts_evt_publish(&evt);
}

ts_res_t ts_safety_poweron_init(void)
{
	for (size_t i = 0; i < ts_ch_count; i++) {
		struct ts_ch_slot *s = &ts_ch_table[i];

		s->state = TS_ST_SAFE_POWERON;
		/* 板级九遗留收口（单元 G1）：注册期 poweron 值落驱动——boot 步骤 2
		 * 时物理输出即达声明的上电安全态（此前仅置 shadow：物理值依赖驱动
		 * 复位缺省与声明一致的隐含假设，声明≠缺省即漂移）。单线程 boot
		 * 上下文（无锁竞争）；直写与 estop/recover 同纪律（不经 commit——
		 * 合同 1 三态落驱动的 poweron 面）。 */
		if (s->desc != NULL) {
			ts_drivers[s->desc->kind].write(s->desc, &s->desc->poweron);
			s->shadow = s->desc->poweron;
		}
	}
	return TS_OK;
}

void ts_safety_set_link(bool up)
{
	/* DEC-43 锁收口：迁移路径与 commit/单通道迁移互斥（set_state 发布事件
	 * 在锁内——锁序 write_lock → pubq，单向）。线程上下文（sysworkq）。 */
	ts_safety_write_lock();
	atomic_set(&ts_link_up, up ? 1 : 0);
	for (size_t i = 0; i < ts_ch_count; i++) {
		struct ts_ch_slot *s = &ts_ch_table[i];

		if (up && (s->state == TS_ST_SAFE_POWERON || s->state == TS_ST_SAFE_LINKLOSS)) {
			/* 链路确立：SAFE_POWERON/Safe_LINKLOSS → ACTIVE（LLD §3 图）。
			 * DR-04：LINKLOSS 恢复不自动回写断链前的值——须显式 commit。 */
			set_state(i, TS_ST_ACTIVE);
		} else if (!up && s->state == TS_ST_ACTIVE) {
			/* 断链：ACTIVE → SAFE_LINKLOSS。声明值落驱动（HLD §4.5-S2；
			 * 板级九修复——此前仅改 shadow，物理输出滞留断链前值 = 合同 3
			 * 欠账）。shadow 改写 + 恢复不回写（DR-04）不变。
			 * IR2-05（DEC-48⑥）：estop ISR 可在持锁检查与写驱动之间抢入
			 * ——irq_lock 末段复查 forced（commit.c IR-02 范式平移）；
			 * 抢入后本通道保持 SAFE_FAULT（fault 语义不丢失）。 */
			if (atomic_get(&ts_forced) != 0) {
				continue;
			}
			unsigned int key = irq_lock();

			if (atomic_get(&ts_forced) == 0) {
				ts_drivers[s->desc->kind].write(s->desc,
								&s->desc->linkloss);
			}
			irq_unlock(key);
			s->shadow = s->desc->linkloss;
			s->have_last = false;
			set_state(i, TS_ST_SAFE_LINKLOSS);
		}
	}
	ts_safety_write_unlock();
}

#ifdef CONFIG_TS_TEST
void ts_safety_test_reset(void)
{
	memset(ts_ch_table, 0, sizeof(ts_ch_table));
	ts_ch_count = 0;
	atomic_set(&ts_forced, 0);
	atomic_set(&ts_forced_at, 0);
	atomic_set(&ts_link_up, 0);
	atomic_set(&ts_arb_owner, TS_ARB_REMOTE);
	ts_safety_estop_announce_reset();
	ts_safety_audit_test_reset(); /* 审计环随复位（CG3：用例序无关的净基线） */
}
#endif

/* ---- 控制源仲裁（CG3 · Q-41②/合同 11 · LLD §5b）------------------------ */

ts_res_t ts_safety_set_arb(ts_arb_owner_t owner)
{
	if (owner != TS_ARB_REMOTE && owner != TS_ARB_LOCAL) {
		return TS_E_PARAM;
	}
	if (k_is_in_isr()) {
		return TS_E_PERM; /* [thread] API（事件发布经总线，ISR 禁入） */
	}
	ts_arb_owner_t old = (ts_arb_owner_t)atomic_get(&ts_arb_owner);

	if (old == owner) {
		return TS_OK; /* 幂等：同席位重复变更零事件零输出（确定性） */
	}
	atomic_set(&ts_arb_owner, owner);
	/* 切换零输出（EN 60204-1 §9.2.3 同型）：本函数不触任何通道写入/
	 * 状态迁移——输出连续性由 APP 逻辑承载；此处仅门控位 + 留痕事件。 */
	const ts_arb_evt_t payload = {
		.owner = (uint8_t)owner,
		.old = (uint8_t)old,
	};
	const ts_evt_t evt = {
		.id = TS_EVT_ARB_CHANGED,
		.t_ms = ts_time_ms(),
		.data = &payload,
		.len = sizeof(payload),
	};
	ts_evt_publish(&evt);
	return TS_OK;
}

ts_arb_owner_t ts_safety_arb_owner(void)
{
	return (ts_arb_owner_t)atomic_get(&ts_arb_owner);
}

ts_res_t ts_safety_force_channel_fault(const char *uid)
{
	int i = ts_ch_find(uid);

	if (i < 0) {
		return TS_E_NOTFOUND;
	}
	/* DEC-43 锁收口：与 commit/迁移互斥 */
	ts_safety_write_lock();
	struct ts_ch_slot *s = &ts_ch_table[i];

	/* 与 force_all_fault 同语义但单通道；不置全局 forced（estop 专用锁存） */
	ts_drivers[s->desc->kind].write(s->desc, &s->desc->fault);
	s->shadow = s->desc->fault;
	s->have_last = false;
	set_state(i, TS_ST_SAFE_FAULT);
	ts_safety_write_unlock();
	return TS_OK;
}

ts_res_t ts_safety_channel_recover(const char *uid)
{
	int i = ts_ch_find(uid);

	if (i < 0) {
		return TS_E_NOTFOUND;
	}
	if (atomic_get(&ts_forced) != 0) {
		return TS_E_STATE; /* estop/system_fail 锁存期不得单通道恢复 */
	}
	/* DEC-43 锁收口：与 commit/迁移互斥 */
	ts_safety_write_lock();
	if (atomic_get(&ts_forced) != 0) {
		ts_safety_write_unlock();
		return TS_E_STATE; /* 双检：持锁后复核（estop 可在两检之间置位） */
	}
	struct ts_ch_slot *s = &ts_ch_table[i];

	/* 物理重附 = 重新上电语义：回 poweron 值；链路已立 → ACTIVE。
	 * IR2-05（DEC-48⑥）：estop ISR 可在第二次检查与写驱动之间抢入——
	 * irq_lock 末段复查 forced；抢入则放弃迁移（通道保持 SAFE_FAULT）。 */
	unsigned int key = irq_lock();
	bool forced_now = atomic_get(&ts_forced) != 0;

	if (!forced_now) {
		ts_drivers[s->desc->kind].write(s->desc, &s->desc->poweron);
	}
	irq_unlock(key);
	if (forced_now) {
		ts_safety_write_unlock();
		return TS_E_STATE;
	}
	s->shadow = s->desc->poweron;
	s->have_last = false;
	set_state(i, atomic_get(&ts_link_up) ? TS_ST_ACTIVE : TS_ST_SAFE_POWERON);
	ts_safety_write_unlock();
	return TS_OK;
}

ts_res_t ts_safety_clear_fault(void)
{
	/* IR-01 修复（M1 自检）：clear_fault 是 estop 锁存的**唯一释放路径**
	 *（LLD §5"clear_fault 前 forced 标志不复位"= 复位只经此处发生）；
	 * 运行期可达性 = 仅 sys:estop-clear（host-only + 确认令牌，DEC-30①）。
	 * 复位语义（F-8 定案，LLD-ts-safety §5 v0.2.1）：状态条件恢复
	 * （link up → ACTIVE，否则 SAFE_LINKLOSS）+ **输出值不回写**——物理
	 * 输出保持 fault 安全值直至下一次显式 commit（与 DR-04"断链恢复不
	 * 自动回写"同则；调用方须重新提交目标值）。 */
	atomic_set(&ts_forced, 0);
	ts_safety_estop_announce_reset();
	/* DEC-43 锁收口：状态批量迁移与 commit/单通道迁移互斥 */
	ts_safety_write_lock();
	for (size_t i = 0; i < ts_ch_count; i++) {
		if (ts_ch_table[i].state == TS_ST_SAFE_FAULT) {
			set_state(i, atomic_get(&ts_link_up) ? TS_ST_ACTIVE : TS_ST_SAFE_LINKLOSS);
			ts_ch_table[i].have_last = false;
		}
	}
	ts_safety_write_unlock();
	return TS_OK;
}

ts_res_t ts_safety_channel_state(const char *uid, ts_ch_state_t *out)
{
	if (out == NULL) {
		return TS_E_PARAM;
	}
	int i = ts_ch_find(uid);

	if (i < 0) {
		return TS_E_NOTFOUND;
	}
	*out = ts_ch_table[i].state;
	return TS_OK;
}

ts_res_t ts_safety_summary(ts_safety_summary_t *out)
{
	if (out == NULL) {
		return TS_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < ts_ch_count; i++) {
		if (ts_ch_table[i].state < TS_CH_STATE_COUNT) {
			out->by_state[ts_ch_table[i].state]++;
		}
	}
	out->channels = (uint16_t)ts_ch_count;
	return TS_OK;
}
