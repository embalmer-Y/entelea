/* SPDX-License-Identifier: Apache-2.0 */
/* entelea safety 内部共享（不对外）：通道槽表/forced/link 原子与锁收口。 */
#ifndef ENTELEA_SAFETY_INTERNAL_H
#define ENTELEA_SAFETY_INTERNAL_H

#include <string.h>
#include <entelea/safety.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct entelea_ch_slot {
	const entelea_out_channel_t *desc;
	entelea_ch_state_t state;
	entelea_out_value_t shadow; /* SAFE_* 态 = 安全值；ACTIVE = 最后落值 */
	bool have_last;             /* slew 基线是否已建立 */
	uint64_t last_t;            /* 上次 commit 时刻（slew Δt） */
	struct {
		uint32_t head, count;
		entelea_write_rec_t ring[ENTELEA_SAFETY_SIM_REC_PER_CH];
	} sim_rec;                 /* 写序列记录（golden 数据源） */
};

extern struct entelea_ch_slot entelea_ch_table[ENTELEA_SAFETY_MAX_CHANNELS];
extern size_t entelea_ch_count;

extern atomic_t entelea_forced;    /* estop/fail 锁存标志（clear_fault 释放） */
extern atomic_t entelea_forced_at; /* forced 置位时刻（低 32 位，补发事件用） */
extern atomic_t entelea_link_up;   /* 全局链路标志 */
extern const entelea_clock_t *entelea_safety_clock;

/* 模块时钟读取（channel.c 实现）：读失败或回退沿用最后有效值（单调
 * 护栏，review 缺陷 5——失败不得伪造 0 导致 slew 减法下溢放宽限制），
 * 失败计入 clock_faults；从未成功过返回 0。 */
uint64_t entelea_safety_now_ms(void);

/* 模块私有驱动 ops 表（drivers.c 定义；模块外引用被架构检查拒绝）。 */
typedef struct {
	int (*write)(const entelea_out_channel_t *ch, const entelea_out_value_t *v);
	int (*read)(const entelea_out_channel_t *ch, entelea_out_value_t *out);
} entelea_driver_ops_t;

extern const entelea_driver_ops_t entelea_drivers[ENTELEA_CH_KIND_COUNT];

/** 驱动写失败计数（channel.c 持有；commit/force 调用点登记）。 */
void entelea_safety_count_write_failure(void);

/** estop 补发宣布位复位（clear_fault 调用；二次 estop 可再补发）。 */
void entelea_safety_estop_announce_reset(void);

/* ---- 锁收口（旧 DEC-43 方案 A 语义）---------------------------------------
 * write_lock = commit_lock 的受控暴露：安全态迁移路径（set_link / 单通道
 * force/recover / clear_fault）须与 commit 互斥。线程上下文专用（ISR 禁入）；
 * 已持锁方调用 commit 须走 commit_locked（k_mutex 非递归）。
 * 锁序：write_lock → 事件发布路径，单向无环。
 * R04 预算检查-提交原子化复用该锁；R02 无外部持有者。 */
void entelea_safety_write_lock(void);
void entelea_safety_write_unlock(void);

/* 查找槽下标；未注册返回 -1 */
static inline int entelea_ch_find(const char *uid)
{
	if (uid == NULL) {
		return -1;
	}
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].desc != NULL &&
		    entelea_ch_table[i].desc->uid != NULL &&
		    strcmp(entelea_ch_table[i].desc->uid, uid) == 0) {
			return (int)i;
		}
	}
	return -1;
}

#endif /* ENTELEA_SAFETY_INTERNAL_H */
