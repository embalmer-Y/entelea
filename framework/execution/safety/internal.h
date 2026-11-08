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
	entelea_out_value_t shadow; /* 最后确认值（写失败不更新） */
	bool have_last;             /* slew 基线是否已建立 */
	uint64_t last_t;            /* 上次确认写时刻（slew Δt 锚点） */
	bool fault_by_write_failure; /* 安全写失败所致 SAFE_FAULT：
				      * 仅 channel_recover 可恢复（clear_fault
				      * 跳过），物理效果未知 */
	struct {
		uint32_t head, count;
		entelea_write_rec_t ring[ENTELEA_SAFETY_SIM_REC_PER_CH];
	} sim_rec;                 /* 写序列记录（golden 数据源） */
};

extern struct entelea_ch_slot entelea_ch_table[ENTELEA_SAFETY_MAX_CHANNELS];
extern size_t entelea_ch_count;

extern atomic_t entelea_forced;    /* estop/fail 锁存标志（clear_fault 释放） */
extern atomic_t entelea_system_failed; /* system_fail 已发生（一次性；clear_fault 拒绝） */
extern atomic_t entelea_forced_at; /* forced 置位时刻（低 32 位，补发事件用） */
extern atomic_t entelea_link_up;   /* 全局链路标志 */
extern atomic_t entelea_arb_owner; /* 控制源席位（R04；entelea_arb_owner_t 值） */
extern const entelea_clock_t *entelea_safety_clock;

/* 模块时钟读取（channel.c 实现）：读失败或回退沿用最后有效值（单调
 * 护栏——失败不得伪造 0 导致 slew 减法下溢放宽限制），失败计入
 * clock_faults；从未成功过返回 0。checked 变体返回 false=本次时间不可知
 * （slew 门与调用方据此拒绝依赖时间界的操作）。 */
uint64_t entelea_safety_now_ms(void);
bool entelea_safety_now_ms_checked(uint64_t *out);

/* 模块私有驱动 ops 表（drivers.c 定义；模块外引用被架构检查拒绝）。 */
typedef struct {
	int (*write)(const entelea_out_channel_t *ch, const entelea_out_value_t *v);
	int (*read)(const entelea_out_channel_t *ch, entelea_out_value_t *out);
} entelea_driver_ops_t;

extern const entelea_driver_ops_t entelea_drivers[ENTELEA_CH_KIND_COUNT];

/** 驱动写失败计数（channel.c 持有；commit/force 调用点登记）。 */
void entelea_safety_count_write_failure(void);

/** 审计落盘（channel.c）：异常类节流（首条立即，其后 1Hz 采样——旧
 * DEC-52① 语义；风暴丢弃计数可观）。persist 失败不反噬提交路径
 * （观测面降级）。 */
void entelea_safety_audit_persist(const void *entry, uint16_t evt);

#ifdef ENTELEA_TEST
/** 后端就绪位/失败计数复位（drivers.c；测试隔离）。 */
void entelea_drv_test_reset(void);
/** system_failed 一次性标志复位（force.c；仅测试隔离）。 */
void entelea_safety_test_reset_system_failed(void);
#endif

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
