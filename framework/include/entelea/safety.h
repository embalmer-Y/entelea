/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_SAFETY_H
#define ENTELEA_SAFETY_H

/* R02 不可绕输出保护合同见 framework/execution/safety/contract.md。
 * 一切产品输出必经本模块唯一写路径；全库唯一 Zephyr 输出驱动调用点是
 * framework/execution/safety/drivers.c（source-policy 保护门）。
 * 相对旧 ts-safety 的裁剪（审计环→R03、控制源仲裁→R04、prov 化 estop
 * 边沿/留痕持久化→R03、通用 GPIO 输入后端→后续批次）记录于合同与台账。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/events.h>
#include <entelea/result.h>
#include <entelea/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 通道模型 ---- */

typedef enum {
	ENTELEA_CH_GPIO,
	ENTELEA_CH_PWM,
	ENTELEA_CH_POWER,
	ENTELEA_CH_KIND_COUNT,
} entelea_ch_kind_t;

typedef union {
	bool b;                /* ENTELEA_CH_GPIO */
	uint32_t u;            /* ENTELEA_CH_PWM */
	struct {
		bool en;
		uint32_t ma;
	} pwr;                 /* ENTELEA_CH_POWER（受控供电） */
} entelea_out_value_t;

typedef struct {
	const char *uid;       /* 稳定逻辑名（注册期冻结） */
	entelea_ch_kind_t kind;
	entelea_out_value_t poweron; /* 三安全态缺一拒绝注册 */
	entelea_out_value_t linkloss;
	entelea_out_value_t fault;
	struct {
		uint32_t min, max;         /* 限幅（GPIO 忽略） */
		uint32_t slew_per_ms;      /* 变化率上界（0 = 不限） */
		uint32_t current_limit_ma; /* 供电限流（POWER） */
	} limits;
} entelea_out_channel_t;

/* 三安全态状态机 */
typedef enum {
	ENTELEA_ST_SAFE_POWERON,
	ENTELEA_ST_ACTIVE,
	ENTELEA_ST_SAFE_LINKLOSS,
	ENTELEA_ST_SAFE_FAULT,
	ENTELEA_CH_STATE_COUNT,
} entelea_ch_state_t;

/** [thread] init 期 + 外设注册期；三态缺一/值越限/uid 非法或撞名 →
 * ENTELEA_E_PARAM（注册即失败，不留半注册）。容量 ENTELEA_SAFETY_MAX_CHANNELS。 */
entelea_result_t entelea_safety_register_channel(const entelea_out_channel_t *ch);

/** 绑定实例时钟（slew Δt / estop 时刻戳来源）；未绑定调用返回
 * ENTELEA_E_UNINIT 的入口在合同中标明；绑定 NULL 恢复未绑定。 */
entelea_result_t entelea_safety_bind_clock(const entelea_clock_t *clock);

/* ---- 唯一写路径（保护强制点） ---- */

/** [thread] 一切输出必经的保护校验出口（限幅→slew→限流→末段临界区）。
 * 非 ACTIVE 态 / fail-safe 锁存期 → ENTELEA_E_STATE；供电超限整笔拒绝
 * → ENTELEA_E_RANGE；限幅/slew 拆分后返回实际落值并报 ENTELEA_E_RANGE。
 * ISR 调用 → ENTELEA_E_PERM。 */
entelea_result_t entelea_safety_commit(const char *uid, entelea_out_value_t v);

/** [thread] 供已持 write_lock 的调用方复用（R04 预算检查-提交原子化路径；
 * R02 无外部持有者，仅供合同完备）。 */
struct entelea_safety_pend;
entelea_result_t entelea_safety_commit_locked(const char *uid,
		entelea_out_value_t v, struct entelea_safety_pend *pend);

/** [any] 影子值读回（不经驱动）。 */
entelea_result_t entelea_safety_readback(const char *uid, entelea_out_value_t *out);

/** [any] 通道当前安全态（观测点）。 */
entelea_result_t entelea_safety_channel_state(const char *uid,
		entelea_ch_state_t *out);

/** entelea_out_value_t 规范单字编码（b→0/1；pwr→en<<31|ma&0x7FFFFFFF；
 * 其余取 .u）。 */
static inline uint32_t entelea_value_encode(entelea_ch_kind_t k,
		entelea_out_value_t v)
{
	switch (k) {
	case ENTELEA_CH_GPIO:
		return v.b ? 1U : 0U;
	case ENTELEA_CH_POWER:
		return (v.pwr.en ? 1U << 31 : 0U) | (v.pwr.ma & 0x7FFFFFFFU);
	default:
		return v.u;
	}
}

/** 汇总观测：按态计数 + 注册总数（无锁读，观测面容忍微撕裂）。 */
typedef struct {
	uint16_t by_state[ENTELEA_CH_STATE_COUNT];
	uint16_t channels;
} entelea_safety_summary_t;

entelea_result_t entelea_safety_summary(entelea_safety_summary_t *out);

/* ---- estop 与 fail-safe 直达 ---- */

/** [ISR] estop GPIO 回调直接调用：原子置 forced → 逐通道直写 fault 值。
 * 调用图内禁分配/队列/锁/协议栈。 */
void entelea_safety_force_all_fault(void);

/** [thread] WDT/BOOT/子系统故障：全通道进 SAFE_FAULT 并停机编排
 * （ENTELEA_TEST 构建置态后返回，由用例断言停机路径）。
 * 停机前经 entelea_safety_record_fail_reason 留痕（存储模块强实现
 * 落 noinit；无存储构建为弱桩——留痕缺失如实呈现）。 */
void entelea_safety_system_fail(uint32_t reason);

/** system_fail 复位原因留痕钩（force.c 弱桩；存储模块 R03 强实现）。 */
void entelea_safety_record_fail_reason(uint32_t reason);

/** [thread] 链路判定迁移（sysworkq 串行化；带滞回归 R06 消费方）。
 * 迁移写失败（后端返回错误）→ ENTELEA_E_IO，不更新影子/状态。 */
entelea_result_t entelea_safety_set_link(bool up);

/* ---- 单通道故障/恢复（detach 语义的机制面） ---- */

/** [thread] 物理不在场 = 单通道 fault（不置全局 estop 锁存）。 */
entelea_result_t entelea_safety_force_channel_fault(const char *uid);

/** [thread] 物理重附 = 重新上电语义（回 poweron 值；全局锁存期拒绝恢复）。 */
entelea_result_t entelea_safety_channel_recover(const char *uid);

/** [thread] estop 锁存唯一释放路径（条件恢复：link up → ACTIVE，否则
 * SAFE_LINKLOSS；输出值不回写——保持 fault 安全值直至下一次显式 commit）。 */
entelea_result_t entelea_safety_clear_fault(void);

/** [thread] estop 事后补发（sysworkq 周期检测 forced 上升沿 → 发布
 * ENTELEA_EVT_ESTOP；ISR 内不提交工作项）。 */
void entelea_safety_estop_deferred_publish(void);

/* ---- boot 步骤实现 ---- */

/** [thread] estop DT 绑定：无节点板（native_sim/测试）空操作返回 OK；
 * 生产板必须提供（板级断言随参考板冻结）。 */
entelea_result_t entelea_safety_estop_init(void);

/** [thread] 全通道置 SAFE_POWERON 初态（声明值落驱动）。 */
entelea_result_t entelea_safety_poweron_init(void);

/** estop 触发边沿映射（0=上升缺省/1=下降/2=双沿；未知 fail-safe 上升）。
 * prov 化配置面归 R03；R02 固定缺省。 */
int entelea_safety_estop_edge_of(uint8_t flags);

/** estop 直达触发闩（ISR 置位、clear_fault 复位）——观测面，无锁原子读
 *（r14 真机闸观测）。 */
bool entelea_safety_estop_latched(void);

/** 最近一次 estop 触发时刻（绑定时钟 ms；未触发=0）。事实记录——
 * clear_fault 只释放闩不清时刻（EVT_ESTOP 边沿发布依赖），观测面
 * 无锁原子读。 */
uint32_t entelea_safety_estop_latched_at(void);

/** estop 引脚原始物理电平（0/1；-1=未绑定或读失败）——接线诊断/
 * 现场观测面（r14：区分"接线未变"与"电平变而沿未触发"）。 */
int entelea_safety_estop_level_raw(void);

/* ---- 事件 payload（本模块发布） ---- */

typedef struct {
	const char *uid;
	int32_t new_state; /* entelea_ch_state_t 值 */
} entelea_safe_state_evt_t;

/* ---- 驱动分发（全库唯一驱动调用点；source-policy 保护门） ---- */

/* 驱动 ops 表是 safety 模块私有实现（internal.h），不对外声明：
 * 公开即产生绕过 commit/限幅/estop 的旁路写入口（review 缺陷 1）。
 * 外部只读观测经 entelea_driver_sim_writes；板级后端装配套经下方
 * entelea_drv_*_init。机械检查禁止模块外引用 entelea_drivers。
 * 写函数返回 0/负 errno：后端失败必须传播到提交结果（缺陷 2）。 */

/* 桩驱动的写序列记录（golden 数据源；真机驱动路径同样记录）。 */
typedef struct {
	uint64_t t_ms;
	uint32_t value_u; /* 编码同 entelea_value_encode */
} entelea_write_rec_t;

/** [thread] 拷贝指定通道写序列快照（从旧到新），返回实际条数。 */
size_t entelea_driver_sim_writes(const char *uid, entelea_write_rec_t *out,
		size_t max);

/* ---- 测试钩子（仅 ENTELEA_TEST 构建） ---- */
#ifdef ENTELEA_TEST
/** 最近一次 system_fail 的 reason（0 = 未发生）。 */
uint32_t entelea_safety_test_fail_reason(void);
/** 通道表/锁存/链路全清（测试隔离；生产不可达；同时复位后端就绪位）。 */
void entelea_safety_test_reset(void);
/** 仅复位后端就绪位/失败计数（未就绪负测用）。 */
void entelea_drv_test_reset(void);
#endif

/* ---- 失败可观测（后端与时钟失败不静默） ---- */

/** [any] 驱动写失败累计（force/estop 路径 ISR 内不可返回，失败留痕于此；
 * commit/poweron 路径失败直接经返回值传播，另计入此计数）。 */
uint32_t entelea_safety_force_write_failures(void);

/** [boot/board] 真机 PWM 输出后端初始化（板级装配面；driver_dispatch.c）。
 * 依赖 ENTELEA_DRV_PWM 编译定义 + zephyr,user 节点（pwm-uid + pwms）；
 * 未启用/缺绑定返回 -ENODEV。 */
int entelea_drv_pwm_init(void);

/** [any] PWM 后端写失败计数（观测面）。 */
uint32_t entelea_drv_pwm_err_count(void);

/** [boot/board] 真机 GPIO 输出后端初始化（ENTELEA_DRV_GPIO +
 * zephyr,user 的 uid + io-gpios）；未启用/缺绑定返回 -ENODEV。 */
int entelea_drv_gpio_init(void);

/** [any] 模块时钟读取失败累计（护栏生效：失败/回退沿用最后有效值）。 */
uint32_t entelea_safety_clock_faults(void);

/** [any] 审计落盘节流丢弃累计（风暴 1Hz 采样期间的观测量）。 */
uint32_t entelea_safety_audit_throttled(void);

/* ---- 控制源仲裁（R04；EN 60204-1 §9.2 同型） ---- */

typedef enum { ENTELEA_ARB_REMOTE = 0, ENTELEA_ARB_LOCAL = 1 } entelea_arb_owner_t;

/* 写路径来源标记：APP=编排意图载体（natives）；REMOTE=远程编排写
 * （压制门控唯一作用面）；LOCAL=本地交互写（未来本地 UI 面）。 */
typedef enum {
	ENTELEA_SRC_APP = 0,
	ENTELEA_SRC_REMOTE = 1,
	ENTELEA_SRC_LOCAL = 2,
} entelea_commit_src_t;

/** [thread] 席位变更：仅改门控位 + 发布 ENTELEA_EVT_ARB_CHANGED——
 * 零通道写入（输出连续性由 APP 逻辑承载）。幂等（同席位零事件）。 */
entelea_result_t entelea_safety_set_arb(entelea_arb_owner_t owner);

/** [any] 当前席主。 */
entelea_arb_owner_t entelea_safety_arb_owner(void);

/** [thread] 带来源的写路径：本地席位激活期拒绝 REMOTE 来源
 * （ENTELEA_E_STATE 留痕）；APP/LOCAL 不受门控。 */
entelea_result_t entelea_safety_commit_src(const char *uid,
					   entelea_out_value_t v,
					   entelea_commit_src_t src);

/* ARB 事件 payload。 */
typedef struct {
	uint8_t owner; /* entelea_arb_owner_t 新席主 */
	uint8_t old;   /* 原席主（幂等变更不发布） */
} entelea_arb_evt_t;

/* 容量出处 DEC-27（旧 LLD-ts-safety §2/§7），R02 为编译期常量。 */
#define ENTELEA_SAFETY_MAX_CHANNELS 32
/* 每通道写序列环形容量（结构性数值）。 */
#define ENTELEA_SAFETY_SIM_REC_PER_CH 32

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_SAFETY_H */
