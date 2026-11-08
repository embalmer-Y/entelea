/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_EVENTS_H
#define ENTELEA_EVENTS_H

/* R02 同步事件总线合同见 framework/runtime/supervision/contract.md。
 * 分发顺序 = 注册表静态顺序（禁运行期插队）；ISR 投递经队列由 sysworkq
 * 分发，投递的 data 必须指向静态存储（借用寿命约束，无机械检查）。 */

#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>
#include <entelea/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R02 事件子集；后续批次按需追加，不改既有编号。 */
typedef enum {
	ENTELEA_EVT_BOOT_STEP,
	ENTELEA_EVT_BOOT_DONE,
	ENTELEA_EVT_ESTOP, /* 事后补发；estop 动作本身不经队列 */
	ENTELEA_EVT_SAFE_STATE_CHANGED,
	ENTELEA_EVT_WDT_WARN,
	ENTELEA_EVT_ID_COUNT,
} entelea_event_id_t;

typedef struct {
	entelea_event_id_t id;
	uint64_t t_ms;
	const void *data;
	size_t len;
} entelea_event_t;

typedef void (*entelea_event_cb_t)(const entelea_event_t *evt, void *user);

/* ENTELEA_EVT_BOOT_STEP payload */
typedef struct {
	uint8_t idx;
	int32_t result;
	const char *name;
} entelea_boot_step_evt_t;

/* ENTELEA_EVT_ESTOP payload（补发时携带 ISR 记录的触发时刻） */
typedef struct {
	uint32_t t_ms;
} entelea_estop_evt_t;

/* ENTELEA_EVT_WDT_WARN payload */
typedef struct {
	int32_t src; /* entelea_wdt_src_t 值 */
	uint32_t last_feed;
} entelea_wdt_warn_evt_t;

/* 绑定实例时钟（发布时间戳来源）。未绑定时发布 t_ms = 0，不伪造默认时基；
 * 订阅/发布其余语义不变。绑定 NULL 恢复未绑定。 */
entelea_result_t entelea_evt_bind_clock(const entelea_clock_t *clock);

/** [thread] 仅 init 期注册；每事件容量 ENTELEA_EVT_MAX_SUBS，满 = ENTELEA_E_NOMEM。 */
entelea_result_t entelea_evt_subscribe(entelea_event_id_t id,
		entelea_event_cb_t fn, void *user);

/** [thread] 同步分发（注册表静态顺序）；[ISR] 投递到深度
 * ENTELEA_EVT_QUEUE_DEPTH 的队列由 sysworkq 分发；满 = 丢弃并计数（观测损失）。
 * ISR 投递的 data 必须指向静态存储。 */
void entelea_evt_publish(const entelea_event_t *evt);

/** [any] 队列溢出累计丢弃数（观测，不清零）。 */
uint32_t entelea_evt_queue_dropped(void);

/* 仅 ENTELEA_TEST 构建：同步排空 ISR 投递队列并分发（测试观测用）。 */
#ifdef ENTELEA_TEST
void entelea_evt_poll_drain(void);
#endif

/* 容量出处 DEC-27（旧 LLD-ts-core §4/§6），R02 为编译期常量；
 * 板级冻结（D12）前不提供 Kconfig 面。 */
#define ENTELEA_EVT_MAX_SUBS 4
#define ENTELEA_EVT_QUEUE_DEPTH 16

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_EVENTS_H */
