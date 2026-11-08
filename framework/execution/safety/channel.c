/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 通道注册表 + 三安全态状态机（迁自旧 safety/channel.c，R02 合同
 * framework/execution/safety/contract.md）。
 * 三安全态声明缺一不予注册；安全参数运行时只读（desc 指向注册期冻结数据）。
 * 相对旧实现调整（含 review 整改）：
 * - 控制源仲裁（set_arb/commit_src/ARB 事件）延至 R04；审计环形延至 R03。
 * - 写驱动 + 影子/状态迁移全部在 irq_lock 临界区内完成，estop ISR 抢入
 *   不再被临界区后的更新覆盖（缺陷 3）；SAFE_STATE_CHANGED 在解锁后发布
 *   且发布前复查 forced（estop 强制迁移不发逐通道事件，权威态以
 *   channel_state/summary 查询为准）。
 * - 驱动写返回值传播：poweron/断链/恢复写失败显式 E_IO，不更新成功
 *   状态（缺陷 2）。激活迁移（→ACTIVE）不落驱动（恢复不回写，DR-04）。
 * - 模块时钟单调护栏：读失败/回退沿用最后有效值并计数（缺陷 5）。
 */
#include <string.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include "internal.h"

struct entelea_ch_slot entelea_ch_table[ENTELEA_SAFETY_MAX_CHANNELS]; /* 容量 32（DEC-27） */
size_t entelea_ch_count;

atomic_t entelea_link_up = ATOMIC_INIT(0); /* 初值 down：boot 后链路未确立 */
atomic_t entelea_arb_owner = ATOMIC_INIT(ENTELEA_ARB_REMOTE); /* 缺省远程（本地席位空缺） */
atomic_t entelea_forced = ATOMIC_INIT(0);
atomic_t entelea_forced_at = ATOMIC_INIT(0);
const entelea_clock_t *entelea_safety_clock;

/* 单调护栏状态：最后有效时钟值与失败计数（review 缺陷 5）。 */
static uint64_t clock_last_good;
static uint32_t clock_fault_count;

/* 驱动写失败累计（commit/poweron/迁移失败另经返回值传播；force 路径
 * ISR 内不可返回，此计数是唯一留痕面）。 */
static uint32_t driver_write_failures;

/* 受检时钟读取：返回 false = 本次时间不可知（护栏介入，*out 为最后
 * 有效值）。并发（二轮 review 缺陷 5）：commit/迁移（线程）与
 * force_all_fault/estop 补发（ISR）并发访问簿记；32 位目标 u64 可撕裂，
 * 比较/更新/计数入 irq_lock 微临界区（ISR 可嵌套）。 */
bool entelea_safety_now_ms_checked(uint64_t *out)
{
	uint64_t ms = 0;
	unsigned int key = irq_lock();

	/* 读+比较+更新全程持锁（读取须短且不阻塞）：线程/ISR（force/
	 * estop 补发）采样串行化，正常前进不被交错误记回退；真回退
	 * 计故障并保守沿用最后有效值（slew 门据此拒绝）。 */
	bool ok = entelea_safety_clock != NULL &&
		  entelea_safety_clock->read_ms != NULL &&
		  entelea_safety_clock->read_ms(entelea_safety_clock->user, &ms) == ENTELEA_OK;

	if (!ok || ms < clock_last_good) {
		clock_fault_count++;
		ms = clock_last_good;
	} else {
		clock_last_good = ms;
	}
	irq_unlock(key);
	*out = ms;
	return ok;
}

uint64_t entelea_safety_now_ms(void)
{
	uint64_t v;

	(void)entelea_safety_now_ms_checked(&v);
	return v;
}

uint32_t entelea_safety_clock_faults(void)
{
	return clock_fault_count;
}

uint32_t entelea_safety_force_write_failures(void)
{
	return driver_write_failures;
}

/* 审计落盘节流（旧 F1/DEC-52①：锁内零 flash；失败提交循环会把异常
 * 下沉放大为写/擦风暴——首条立即，其后 1Hz 采样）。 */
#define ENTELEA_SAFETY_AUDIT_MIN_MS 1000
static uint32_t audit_last_persist_ms;
static uint32_t audit_throttled_count;

uint32_t entelea_safety_audit_throttled(void)
{
	return audit_throttled_count;
}

void entelea_safety_audit_persist(const void *entry, uint16_t evt)
{
	const entelea_storage_audit_entry_t *e = entry;
	uint32_t now = (uint32_t)entelea_safety_now_ms();

	if (audit_last_persist_ms != 0 &&
	    (uint32_t)(now - audit_last_persist_ms) < ENTELEA_SAFETY_AUDIT_MIN_MS) {
		audit_throttled_count++;
		return;
	}
	audit_last_persist_ms = now;
	/* 失败不反噬提交路径（观测面降级——内存/读面另行可观）。 */
	(void)entelea_storage_audit_append(e, evt);
}

/** 模块内调用点（commit/force 路径）登记驱动写失败。 */
void entelea_safety_count_write_failure(void)
{
	driver_write_failures++;
}

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

/* 发布运行订阅者回调：只能在临界区外、且复查未 forced 时调用
 * （forced 窗口内不发布过期迁移事件）。 */
static void publish_state_change(size_t i, entelea_ch_state_t next)
{
	const struct entelea_ch_slot *s = &entelea_ch_table[i];
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

/** 写驱动 + 影子/状态迁移，须持 irq_lock（estop 抢入窗口闭合）。
 * 返回 0=完成；1=forced 抢入（保持 SAFE_FAULT）；-1=后端写失败。
 * A2（owner 2026-11-08 定案）：安全态写失败 = 物理效果未知 → 通道进
 * SAFE_FAULT（撤销输出资格，后续 commit 拒绝；recover 显式恢复）。
 * 影子保留最后确认值（不伪称安全值已落驱）；恢复须 recover 重新写。 */
static int write_transition_locked(struct entelea_ch_slot *s,
		entelea_out_value_t value, entelea_ch_state_t next)
{
	if (atomic_get(&entelea_forced) != 0) {
		return 1;
	}
	if (entelea_drivers[s->desc->kind].write(s->desc, &value) != 0) {
		driver_write_failures++;
		s->state = ENTELEA_ST_SAFE_FAULT;
		s->fault_by_write_failure = true; /* 仅 channel_recover 可恢复 */
		/* 基线保留：失败=无确认写，锚点仍是最后一次确认值+时刻。 */
		return -1;
	}
	s->shadow = value;
	s->fault_by_write_failure = false;
	/* 确认写刷新 slew 锚点（决定 C）：安全迁移不再重置基线——输出
	 * 永远从最后确认值受控爬升；estop-clear 循环不再产生免跳。
	 * now_ms 内部 irq_lock 可嵌套。 */
	s->last_t = entelea_safety_now_ms();
	s->have_last = true;
	s->state = next;
	return 0;
}

/** 仅状态迁移（激活面：不落驱动——恢复不回写，DR-04），须持 irq_lock。
 * 基线不动（激活无物理写，最后确认值仍有效）。返回 0=完成；1=forced。 */
static int activate_locked(struct entelea_ch_slot *s)
{
	if (atomic_get(&entelea_forced) != 0) {
		return 1;
	}
	s->state = ENTELEA_ST_ACTIVE;
	return 0;
}

entelea_result_t entelea_safety_poweron_init(void)
{
	bool any_fail = false;

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		/* 注册期 poweron 值落驱动：boot 步骤 2 时物理输出即达声明的
		 * 上电安全态。estop 可在 estop_init（步骤 1）之后任意时刻抢入：
		 * forced 则跳过，保持 ISR 置的 SAFE_FAULT（缺陷 3）；写失败
		 * 聚合上报 → boot fail-safe（缺陷 2；物理未知由 fail-safe 收口）。 */
		unsigned int key = irq_lock();
		int rc = write_transition_locked(s, s->desc->poweron,
						 ENTELEA_ST_SAFE_POWERON);

		irq_unlock(key);
		if (rc < 0) {
			any_fail = true;
		}
	}
	return any_fail ? ENTELEA_E_IO : ENTELEA_OK;
}

entelea_result_t entelea_safety_set_link(bool up)
{
	bool any_fail = false;

	/* 锁收口：迁移路径与 commit/单通道迁移互斥（线程上下文）。 */
	entelea_safety_write_lock();
	atomic_set(&entelea_link_up, up ? 1 : 0);
	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (up && (s->state == ENTELEA_ST_SAFE_POWERON ||
			   s->state == ENTELEA_ST_SAFE_LINKLOSS)) {
			/* 激活：不落驱动（恢复不自动回写断链前的值）。 */
			unsigned int key = irq_lock();
			int rc = activate_locked(s);

			irq_unlock(key);
			if (rc == 0 && atomic_get(&entelea_forced) == 0) {
				publish_state_change(i, ENTELEA_ST_ACTIVE);
			}
		} else if (!up && s->state == ENTELEA_ST_ACTIVE) {
			/* 断链：声明值落驱动 + 迁移，全部在临界区内（缺陷 3）。 */
			unsigned int key = irq_lock();
			int rc = write_transition_locked(s, s->desc->linkloss,
							 ENTELEA_ST_SAFE_LINKLOSS);

			irq_unlock(key);
			if (rc < 0) {
				any_fail = true;
			} else if (rc == 0 && atomic_get(&entelea_forced) == 0) {
				publish_state_change(i, ENTELEA_ST_SAFE_LINKLOSS);
			}
		}
	}
	entelea_safety_write_unlock();
	return any_fail ? ENTELEA_E_IO : ENTELEA_OK;
}

entelea_result_t entelea_safety_set_arb(entelea_arb_owner_t owner)
{
	if (owner != ENTELEA_ARB_REMOTE && owner != ENTELEA_ARB_LOCAL) {
		return ENTELEA_E_PARAM;
	}
	if (k_is_in_isr()) {
		return ENTELEA_E_PERM; /* [thread] API（事件发布经总线） */
	}
	entelea_arb_owner_t old = (entelea_arb_owner_t)atomic_get(&entelea_arb_owner);

	if (old == owner) {
		return ENTELEA_OK; /* 幂等：零事件零输出 */
	}
	atomic_set(&entelea_arb_owner, owner);
	/* 切换零输出（输出连续性由 APP 逻辑承载）；仅门控位+留痕事件。 */
	const entelea_arb_evt_t payload = {
		.owner = (uint8_t)owner,
		.old = (uint8_t)old,
	};
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_ARB_CHANGED,
		.data = &payload,
		.len = sizeof(payload),
	};
	entelea_evt_publish(&evt);
	return ENTELEA_OK;
}

entelea_arb_owner_t entelea_safety_arb_owner(void)
{
	return (entelea_arb_owner_t)atomic_get(&entelea_arb_owner);
}

#ifdef ENTELEA_TEST
void entelea_safety_test_reset(void)
{
	memset(entelea_ch_table, 0, sizeof(entelea_ch_table));
	entelea_ch_count = 0;
	atomic_set(&entelea_forced, 0);
	atomic_set(&entelea_forced_at, 0);
	atomic_set(&entelea_link_up, 0);
	atomic_set(&entelea_arb_owner, ENTELEA_ARB_REMOTE);
	clock_last_good = 0;
	clock_fault_count = 0;
	driver_write_failures = 0;
	audit_last_persist_ms = 0;
	audit_throttled_count = 0;
	entelea_drv_test_reset();
	entelea_safety_test_reset_system_failed();
	entelea_safety_estop_announce_reset();
}
#endif

entelea_result_t entelea_safety_force_channel_fault(const char *uid)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	/* 锁收口：与 commit/迁移互斥 */
	entelea_safety_write_lock();
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	entelea_ch_state_t prev = s->state;
	unsigned int key = irq_lock();
	int rc = write_transition_locked(s, s->desc->fault, ENTELEA_ST_SAFE_FAULT);

	irq_unlock(key);
	entelea_safety_write_unlock();
	if (rc < 0) {
		return ENTELEA_E_IO; /* 后端失败：状态未迁移，不伪造 detach 成功 */
	}
	if (rc == 0 && prev != ENTELEA_ST_SAFE_FAULT &&
	    atomic_get(&entelea_forced) == 0) {
		publish_state_change(i, ENTELEA_ST_SAFE_FAULT);
	}
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
		return ENTELEA_E_STATE; /* 双检：持锁后复核 */
	}
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	entelea_ch_state_t next =
		atomic_get(&entelea_link_up) ? ENTELEA_ST_ACTIVE : ENTELEA_ST_SAFE_POWERON;

	/* 物理重附 = 重新上电语义：回 poweron 值（写+迁移在临界区内，缺陷 3）。 */
	unsigned int key = irq_lock();
	int rc = write_transition_locked(s, s->desc->poweron, next);

	irq_unlock(key);
	entelea_safety_write_unlock();
	if (rc < 0) {
		return ENTELEA_E_IO;
	}
	if (rc == 0 && atomic_get(&entelea_forced) == 0) {
		publish_state_change(i, next);
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_clear_fault(void)
{
	/* estop 锁存的唯一释放路径（运行期可达性归后续命令面；R02 直调仅供
	 * 测试）。释放语义：状态条件恢复（link up → ACTIVE，否则
	 * SAFE_LINKLOSS）+ 输出值不回写——物理输出保持 fault 安全值直至
	 * 下一次显式 commit。estop 可在释放后任意时刻再抢入：逐通道在
	 * 临界区内复查 forced（缺陷 3）。
	 * 释放资格（三轮 review 发现 1）：system_fail 是终态（halt 一次性、
	 * 停机语义），其后 clear_fault 拒绝；安全写失败所致 SAFE_FAULT 的
	 * 通道（物理未知）不在本路径恢复——仅显式 channel_recover。 */
	if (atomic_get(&entelea_system_failed) != 0) {
		return ENTELEA_E_STATE; /* system_fail 后不得恢复输出 */
	}
	atomic_set(&entelea_forced, 0);
	entelea_safety_estop_announce_reset();
	/* 锁收口：状态批量迁移与 commit/单通道迁移互斥 */
	entelea_safety_write_lock();
	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (s->state != ENTELEA_ST_SAFE_FAULT || s->fault_by_write_failure) {
			continue; /* 写失败通道仅 recover 可恢复 */
		}
		entelea_ch_state_t next = atomic_get(&entelea_link_up)
						  ? ENTELEA_ST_ACTIVE
						  : ENTELEA_ST_SAFE_LINKLOSS;
		unsigned int key = irq_lock();

		if (atomic_get(&entelea_forced) == 0) {
			s->state = next; /* 无物理写：基线保留（决定 C） */
		}
		irq_unlock(key);
		if (atomic_get(&entelea_forced) == 0) {
			publish_state_change(i, next);
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
