/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 通道注册表 + 三安全态状态机（迁自旧 safety/channel.c，R02 合同
 * framework/execution/safety/contract.md）。
 * 三安全态声明缺一不予注册；安全参数运行时只读（desc 指向注册期冻结数据）。
 * 相对旧实现调整：控制源仲裁（set_arb/commit_src/ARB 事件）延至 R04；
 * 审计环形延至 R03。
 */
#include <string.h>
#include <entelea/safety.h>
#include "internal.h"

struct entelea_ch_slot entelea_ch_table[ENTELEA_SAFETY_MAX_CHANNELS]; /* 容量 32（DEC-27） */
size_t entelea_ch_count;

atomic_t entelea_link_up = ATOMIC_INIT(0); /* 初值 down：boot 后链路未确立 */
atomic_t entelea_forced = ATOMIC_INIT(0);
atomic_t entelea_forced_at = ATOMIC_INIT(0);
const entelea_clock_t *entelea_safety_clock;

static bool value_within_limits(const entelea_out_channel_t *ch,
		entelea_out_value_t v)
{
	switch (ch->kind) {
	case ENTELEA_CH_GPIO:
		return true;
	case ENTELEA_CH_PWM:
		return v.u >= ch->limits.min && v.u <= ch->limits.max;
	case ENTELEA_CH_POWER:
		if (!v.pwr.en) {
			return true;
		}
		return v.pwr.ma <= ch->limits.current_limit_ma;
	default:
		return false;
	}
}

entelea_result_t entelea_safety_register_channel(const entelea_out_channel_t *ch)
{
	if (ch == NULL || ch->uid == NULL || ch->uid[0] == '\0' ||
	    (int)ch->kind < 0 || ch->kind >= ENTELEA_CH_KIND_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (entelea_ch_find(ch->uid) >= 0) {
		return ENTELEA_E_PARAM; /* uid 撞名 */
	}
	if (entelea_ch_count >= ENTELEA_SAFETY_MAX_CHANNELS) {
		return ENTELEA_E_NOMEM;
	}
	/* 三安全态缺一拒绝；且安全态值本身必须落在 limits 内。 */
	if (!value_within_limits(ch, ch->poweron) ||
	    !value_within_limits(ch, ch->linkloss) ||
	    !value_within_limits(ch, ch->fault)) {
		return ENTELEA_E_PARAM;
	}

	struct entelea_ch_slot *slot = &entelea_ch_table[entelea_ch_count++];

	slot->desc = ch;
	slot->state = ENTELEA_ST_SAFE_POWERON;
	slot->shadow = ch->poweron;
	slot->have_last = false;
	slot->last_t = 0;
	slot->sim_rec.head = 0;
	slot->sim_rec.count = 0;
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_bind_clock(const entelea_clock_t *clock)
{
	if (clock == NULL) {
		entelea_safety_clock = NULL; /* 测试隔离恢复 */
		return ENTELEA_OK;
	}
	if (clock->read_ms == NULL || clock->clock_id == 0) {
		return ENTELEA_E_PARAM;
	}
	entelea_safety_clock = clock;
	return ENTELEA_OK;
}

static void set_state(size_t i, entelea_ch_state_t next)
{
	struct entelea_ch_slot *s = &entelea_ch_table[i];

	if (s->state == next) {
		return;
	}
	s->state = next;
	const entelea_safe_state_evt_t payload = {
		.uid = s->desc->uid,
		.new_state = (int32_t)next,
	};
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_SAFE_STATE_CHANGED,
		.data = &payload,
		.len = sizeof(payload),
	};
	entelea_evt_publish(&evt);
}

entelea_result_t entelea_safety_poweron_init(void)
{
	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		s->state = ENTELEA_ST_SAFE_POWERON;
		/* 注册期 poweron 值落驱动：boot 步骤 2 时物理输出即达声明的
		 * 上电安全态（声明≠驱动复位缺省即漂移）。单线程 boot 上下文
		 * （无锁竞争）；直写不经 commit——三态落驱动的 poweron 面。 */
		if (s->desc != NULL) {
			entelea_drivers[s->desc->kind].write(s->desc, &s->desc->poweron);
			s->shadow = s->desc->poweron;
		}
	}
	return ENTELEA_OK;
}

void entelea_safety_set_link(bool up)
{
	/* 锁收口：迁移路径与 commit/单通道迁移互斥（set_state 发布事件在锁内
	 * ——锁序 write_lock → 事件发布，单向）。线程上下文（sysworkq）。 */
	entelea_safety_write_lock();
	atomic_set(&entelea_link_up, up ? 1 : 0);
	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (up && (s->state == ENTELEA_ST_SAFE_POWERON ||
			   s->state == ENTELEA_ST_SAFE_LINKLOSS)) {
			/* 链路确立：SAFE_POWERON/SAFE_LINKLOSS → ACTIVE。
			 * 恢复不自动回写断链前的值——须显式 commit。 */
			set_state(i, ENTELEA_ST_ACTIVE);
		} else if (!up && s->state == ENTELEA_ST_ACTIVE) {
			/* 断链：ACTIVE → SAFE_LINKLOSS。声明值落驱动；
			 * shadow 改写 + 恢复不回写。estop ISR 可在持锁检查与写
			 * 驱动之间抢入——irq_lock 末段复查 forced；抢入后本通道
			 * 保持 SAFE_FAULT（fault 语义不丢失）。 */
			if (atomic_get(&entelea_forced) != 0) {
				continue;
			}
			unsigned int key = irq_lock();

			if (atomic_get(&entelea_forced) == 0) {
				entelea_drivers[s->desc->kind].write(s->desc,
								     &s->desc->linkloss);
			}
			irq_unlock(key);
			s->shadow = s->desc->linkloss;
			s->have_last = false;
			set_state(i, ENTELEA_ST_SAFE_LINKLOSS);
		}
	}
	entelea_safety_write_unlock();
}

entelea_result_t entelea_safety_force_channel_fault(const char *uid)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	/* 锁收口：与 commit/迁移互斥 */
	entelea_safety_write_lock();
	struct entelea_ch_slot *s = &entelea_ch_table[i];

	/* 与 force_all_fault 同语义但单通道；不置全局 forced（estop 专用锁存）。 */
	entelea_drivers[s->desc->kind].write(s->desc, &s->desc->fault);
	s->shadow = s->desc->fault;
	s->have_last = false;
	set_state(i, ENTELEA_ST_SAFE_FAULT);
	entelea_safety_write_unlock();
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_channel_recover(const char *uid)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	if (atomic_get(&entelea_forced) != 0) {
		return ENTELEA_E_STATE; /* estop/system_fail 锁存期不得单通道恢复 */
	}
	/* 锁收口：与 commit/迁移互斥 */
	entelea_safety_write_lock();
	if (atomic_get(&entelea_forced) != 0) {
		entelea_safety_write_unlock();
		return ENTELEA_E_STATE; /* 双检：持锁后复核（estop 可在两检之间置位） */
	}
	struct entelea_ch_slot *s = &entelea_ch_table[i];

	/* 物理重附 = 重新上电语义：回 poweron 值；链路已立 → ACTIVE。
	 * estop ISR 可在第二次检查与写驱动之间抢入——irq_lock 末段复查
	 * forced；抢入则放弃迁移（通道保持 SAFE_FAULT）。 */
	unsigned int key = irq_lock();
	bool forced_now = atomic_get(&entelea_forced) != 0;

	if (!forced_now) {
		entelea_drivers[s->desc->kind].write(s->desc, &s->desc->poweron);
	}
	irq_unlock(key);
	if (forced_now) {
		entelea_safety_write_unlock();
		return ENTELEA_E_STATE;
	}
	s->shadow = s->desc->poweron;
	s->have_last = false;
	set_state(i, atomic_get(&entelea_link_up) ? ENTELEA_ST_ACTIVE
						  : ENTELEA_ST_SAFE_POWERON);
	entelea_safety_write_unlock();
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_clear_fault(void)
{
	/* estop 锁存的唯一释放路径（运行期可达性归后续命令面；R02 直调仅供
	 * 测试）。释放语义：状态条件恢复（link up → ACTIVE，否则
	 * SAFE_LINKLOSS）+ 输出值不回写——物理输出保持 fault 安全值直至
	 * 下一次显式 commit。 */
	atomic_set(&entelea_forced, 0);
	entelea_safety_estop_announce_reset();
	/* 锁收口：状态批量迁移与 commit/单通道迁移互斥 */
	entelea_safety_write_lock();
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].state == ENTELEA_ST_SAFE_FAULT) {
			set_state(i, atomic_get(&entelea_link_up)
					     ? ENTELEA_ST_ACTIVE
					     : ENTELEA_ST_SAFE_LINKLOSS);
			entelea_ch_table[i].have_last = false;
		}
	}
	entelea_safety_write_unlock();
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_channel_state(const char *uid,
		entelea_ch_state_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	*out = entelea_ch_table[i].state;
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_summary(entelea_safety_summary_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if ((int)entelea_ch_table[i].state >= 0 &&
		    entelea_ch_table[i].state < ENTELEA_CH_STATE_COUNT) {
			out->by_state[entelea_ch_table[i].state]++;
		}
	}
	out->channels = (uint16_t)entelea_ch_count;
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_safety_test_reset(void)
{
	memset(entelea_ch_table, 0, sizeof(entelea_ch_table));
	entelea_ch_count = 0;
	atomic_set(&entelea_forced, 0);
	atomic_set(&entelea_forced_at, 0);
	atomic_set(&entelea_link_up, 0);
	entelea_safety_estop_announce_reset();
}
#endif
