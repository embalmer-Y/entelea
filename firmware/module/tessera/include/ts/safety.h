/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ts-safety 公共 API（design/LLD-ts-safety.md）：输出保护层 / 三安全态 / estop / fail-safe。
 * 合同关联：合同 1（三安全态）、2（唯一写路径）、5（estop 不经队列）、7（供电同轨）、8（本地独立）。
 */
#ifndef TS_SAFETY_H__
#define TS_SAFETY_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/core.h> /* safety→core 向下依赖（LLD-ts-common §1）：事件发布/时间源 */
#include <ts/err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 通道模型（LLD-ts-safety §2）---------------------------------------- */

typedef enum {
	TS_CH_GPIO,
	TS_CH_PWM,
	TS_CH_POWER,
	TS_CH_KIND_COUNT,
} ts_ch_kind_t;

typedef union {
	bool b;                            /* TS_CH_GPIO */
	uint32_t u;                        /* TS_CH_PWM */
	struct {
		bool en;
		uint32_t ma;
	} pwr;                             /* TS_CH_POWER（受控供电，DEC-03，合同7） */
} ts_out_value_t;

typedef struct {
	const char *uid;    /* 稳定逻辑名（ts-periph 分配，M3 交付） */
	ts_ch_kind_t kind;
	ts_out_value_t poweron; /* 三安全态：合同1，缺一拒绝注册 */
	ts_out_value_t linkloss;
	ts_out_value_t fault;
	struct {
		uint32_t min, max;         /* 限幅（TS_CH_GPIO 时忽略） */
		uint32_t slew_per_ms;      /* 变化率上界（0 = 不限） */
		uint32_t current_limit_ma; /* 供电限流（TS_CH_POWER） */
	} limits;
} ts_out_ch_t;

/* 三安全态状态机（LLD-ts-safety §3） */
typedef enum {
	TS_ST_SAFE_POWERON,
	TS_ST_ACTIVE,
	TS_ST_SAFE_LINKLOSS,
	TS_ST_SAFE_FAULT,
	TS_CH_STATE_COUNT,
} ts_ch_state_t;

/** [thread] init 期 + 外设注册期；三态缺一/越限/uid 撞名 → TS_E_PARAM（注册即失败，
 * 不留半注册）。容量 CONFIG_TS_SAFETY_MAX_CHANNELS（DEC-27: 32）。 */
ts_res_t ts_safety_register_channel(const ts_out_ch_t *ch);

/* ---- 唯一写路径（LLD-ts-safety §4）——合同 2 强制点 ----------------------- */

/** [thread] 一切输出必经的保护校验出口（限幅→slew→限流→末段临界区→审计）。
 * 非 ACTIVE 态 → TS_E_STATE；限流拒绝 → TS_E_RANGE；slew 拆分后返回实际落值。 */
ts_res_t ts_safety_commit(const char *uid, ts_out_value_t v);

/** [thread] 同 ts_safety_commit，审计 actor 归因（IR2-08/DEC-48⑥）：ts-hal
 * 侧传入调用方 app_id（ts_ctx_opaque.app_id）；actor=0 = system。 */
ts_res_t ts_safety_commit_a(const char *uid, ts_out_value_t v, uint16_t actor);

/* ---- 控制源仲裁状态（CG3 · Q-41②/合同 11 · LLD §5b）--------------------- */

typedef enum { TS_ARB_REMOTE = 0, TS_ARB_LOCAL = 1 } ts_arb_owner_t;

/* 写路径来源标记（§4 commit 入口）：TS_SRC_APP = natives（APP = 编排意图
 * 载体，V1 不经仲裁闸）；TS_SRC_ZENOH = 远程编排写（压制门控唯一作用面）；
 * TS_SRC_LOCAL = 本地交互写（未来本地 UI 面）。 */
typedef enum { TS_SRC_APP = 0, TS_SRC_ZENOH = 1, TS_SRC_LOCAL = 2 } ts_src_t;

/** [thread] 席位变更：仅改门控位 + 发布 TS_EVT_ARB_CHANGED——**零通道写入**
 *（EN 60204-1 §9.2.3 同型：切换不产生输出；连续性由 APP 逻辑承载）。
 * 幂等（同席位重复变更不产事件——确定性）。 */
ts_res_t ts_safety_set_arb(ts_arb_owner_t owner);

/** [any] 当前席主（远程编排 | 本地交互源）。 */
ts_arb_owner_t ts_safety_arb_owner(void);

/** [thread] 带来源标记的写路径：本地席位激活期（arb_owner == LOCAL）拒绝
 * TS_SRC_ZENOH 来源请求（TS_E_STATE 留痕——EN 60204-1 §9.2.4「激活站
 * 独占引发权」同型）；APP/LOCAL 来源不受门控。 */
ts_res_t ts_safety_commit_src(const char *uid, ts_out_value_t v, ts_src_t src);

/** [any] 影子值读回（不经驱动）。 */
ts_res_t ts_safety_readback(const char *uid, ts_out_value_t *out);

/** [any] 通道当前安全态（观测点：重放比对/遥测）。 */
ts_res_t ts_safety_channel_state(const char *uid, ts_ch_state_t *out);

/** ts_out_value_t 的规范单字编码（审计/遥测统一口径；b→0/1，
 * pwr→en<<31|ma&0x7FFFFFFF，其余取 .u）。 */
static inline uint32_t ts_value_encode(ts_ch_kind_t k, ts_out_value_t v)
{
	switch (k) {
	case TS_CH_GPIO:
		return v.b ? 1U : 0U;
	case TS_CH_POWER:
		return (v.pwr.en ? 1U << 31 : 0U) | (v.pwr.ma & 0x7FFFFFFFU);
	default:
		return v.u;
	}
}

/** 汇总观测（sys 命令 get-safety / 遥测，M3a.2）：按态计数 + 注册总数。 */
typedef struct {
	uint16_t by_state[TS_CH_STATE_COUNT];
	uint16_t channels;
} ts_safety_summary_t;

/** [thread] 汇总快照（注册表遍历，无锁读——观测面容忍微撕裂）。 */
ts_res_t ts_safety_summary(ts_safety_summary_t *out);

/* ---- estop 与 fail-safe 直达（LLD-ts-safety §5）——合同 5/8 --------------- */

/** [thread] estop 触发边沿映射（prov estop_trigger_flags → GPIO 边沿；
 * 未知值 fail-safe 上升）。前置声明：driver_dispatch.c 内 estop 初始化
 * 先用后定义（真机 estop 面编译依赖——批 A 续实证修复：native 面 DT 无
 * estop 节点编不到该路径，存量缺陷自单元 F 引入起未被构建面覆盖）。 */
int ts_safety_estop_edge_of(uint8_t flags);

/** [ISR] estop GPIO 回调直接调用：原子置 forced → 逐通道直写 fault 值。
 * 调用图内禁分配/队列/锁/协议栈（L5 机械检查目标）。 */
void ts_safety_force_all_fault(void);

/** [thread] WDT/BOOT/子系统故障：全通道进 SAFE_FAULT 并停机编排。 */
void ts_safety_system_fail(uint32_t reason);

/** [thread] ts-net 专用链路判定（经 sysworkq 串行化迁移；带滞回，M3 细化）。 */
void ts_safety_set_link(bool up);

/* ---- 单通道故障/恢复（M3b，LLD-ts-periph §3 DETACH 语义的机制面）---------
 * 物理不在场 = 故障态：detach 强制单通道 fault（不置全局 estop 锁存）；
 * attach 恢复 = 上电态重放（全局 forced 锁存期拒绝恢复）。 */
ts_res_t ts_safety_force_channel_fault(const char *uid);
ts_res_t ts_safety_channel_recover(const char *uid);

#ifdef CONFIG_TS_TEST
/* 通道表/锁存/链路全清（测试隔离——模块级 test_reset 级联的显式起点；
 * 生产不可达）。 */
void ts_safety_test_reset(void);
#endif

/** [thread] 仅 sys:estop-clear 命令可达（host-only + 确认令牌，DEC-30①）。
 * M1：直调仅供测试；M3 起 net 命令面为唯一运行期入口。forced 标志不复位前拒绝。 */
ts_res_t ts_safety_clear_fault(void);

/** [thread] estop 事后补发（sysworkq 周期检测 forced 上升沿 → 发布 TS_EVT_ESTOP；
 * ISR 内不提交工作项——estop 调用图队列禁令的落点，LLD §5）。 */
void ts_safety_estop_deferred_publish(void);

/* ---- 审计环形（LLD-ts-safety §4-7，DR-07/DEC-30②④）---------------------- */

typedef struct {
	uint64_t t_ms;
	uint16_t ch_idx;   /* 注册表下标；0xFFFF = 全通道事件 */
	int32_t res;       /* commit 结果（TS_OK 或 TS_E_*） */
	uint16_t actor;    /* 调用者 app_id（M1 恒 0=system；M2 起 ts-hal 注入） */
	uint8_t kind;      /* ts_ch_kind_t（值语义按 kind 解释，原始 union 存 value） */
	uint8_t src;       /* ts_src_t 写路径来源（CG3 · 合同 11 审计维度；旧记录 0=APP） */
	uint32_t value_u;  /* ts_out_value_t 的规范单字表示（b→0/1；pwr→en<<31|ma 截低 31 位） */
} ts_audit_entry_t;

/** [thread] 拷贝审计快照（从最新往回 max 条），返回实际拷贝数；溢出丢弃计数另查。 */
size_t ts_safety_audit_copy(ts_audit_entry_t *out, size_t max);

/** [any] 审计溢出累计丢弃条数（观测，不清零）。 */
uint32_t ts_safety_audit_dropped(void);

/* ---- boot 步骤实现（LLD-ts-core §2 由本模块承接的两步）------------------- */

/** [thread] estop DT 绑定（DR-11）：chosen ts,estop-gpio → 配置 IRQ；
 * 无节点板（native_sim/测试）= 空操作返回 TS_OK（生产板必须提供，板级断言 M2+）。 */
ts_res_t ts_safety_estop_init(void);

/** [thread] 全通道置 SAFE_POWERON 初态（boot 步骤 2）。 */
ts_res_t ts_safety_poweron_init(void);

/* ---- 事件 payload（本模块发布的事件）------------------------------------- */

typedef struct {
	const char *uid;
	ts_ch_state_t new_state;
} ts_safe_state_evt_t; /* TS_EVT_SAFE_STATE_CHANGED（LLD §3：uid + 新态） */

typedef struct {
	uint32_t t_ms; /* estop 触发时刻（ISR 记录，补发时带上，合同 5） */
} ts_estop_evt_t; /* TS_EVT_ESTOP */

typedef struct {
	uint8_t owner; /* ts_arb_owner_t 新席主 */
	uint8_t old;   /* 原席主（幂等变更不产事件——owner == old 不发布） */
} ts_arb_evt_t; /* TS_EVT_ARB_CHANGED（CG3 · 合同 11：切换零输出，仅留痕） */

/* ---- 测试钩子（仅 CONFIG_TS_TEST）---------------------------------------- */
#ifdef CONFIG_TS_TEST
/** 最近一次 system_fail 的 reason（测试断言用；0 = 未发生）。 */
uint32_t ts_safety_test_fail_reason(void);
/** 审计环形归零（测试隔离——ts_safety_test_reset 内部调用；CG3 起用例序
 * 不再影响丢弃算术）。 */
void ts_safety_audit_test_reset(void);
#endif

/* ---- 驱动分发（LLD-ts-safety §6）——全库唯一驱动调用点 -------------------- */

typedef struct {
	void (*write)(const ts_out_ch_t *ch, const ts_out_value_t *v);
	int (*read)(const ts_out_ch_t *ch, ts_out_value_t *out);
} ts_driver_ops_t;

extern const ts_driver_ops_t ts_drivers[TS_CH_KIND_COUNT];

/* native_sim 桩驱动的写序列记录（L4 重放 golden 数据源；真机驱动无记录）。 */
typedef struct {
	uint64_t t_ms;
	uint32_t value_u; /* 编码同 ts_audit_entry_t.value_u */
} ts_write_rec_t;

/** [thread] 拷贝指定通道的写序列快照（从旧到新），返回实际条数（环形容量 32/通道）。 */
size_t ts_driversim_writes(const char *uid, ts_write_rec_t *out, size_t max);

/** [boot/board] 真机 GPIO 输出后端初始化（板级步骤；driver_dispatch.c）。
 * 依赖 CONFIG_TS_DRV_GPIO=y + zephyr,user 节点（uid + io-gpios）；
 * 未启用/缺绑定返回 -ENODEV——板级按需如实检查。 */
int ts_drv_gpio_init(void);

/** [boot/board] 真机 PWM 输出后端初始化（板级九；driver_dispatch.c）。
 * 依赖 CONFIG_TS_DRV_PWM=y + zephyr,user 节点（pwm-uid + pwms）；
 * 未启用/缺绑定返回 -ENODEV。 */
int ts_drv_pwm_init(void);

/** [boot/board] 真机 GPIO 输入后端初始化（V0.4.0 M5/DR-02；driver_dispatch.c）。
 * 依赖 CONFIG_TS_DRV_GPIO=y + zephyr,user 节点（gpio-in-uid + in-gpios）；
 * 未启用/缺绑定返回 -ENODEV。边沿模式（CONFIG_TS_HAL_INPUT_EDGE）下注册
 * 双沿中断——回调 ISR 纯净（L5），仅经通知钩子上抛，去抖与重轮询在
 * hal/input.c（唯一发布路径，合同 9）。 */
int ts_drv_gpio_in_init(void);

/** [any] 真机 GPIO 输入电平直读（输入不经保护层——合同 3）；未绑定恒 false。 */
bool ts_drv_gpio_in_read(void);

/** [any] GPIO 输入绑定的 uid（未绑定 = NULL）——api 层 uid 匹配用。 */
const char *ts_drv_gpio_in_uid(void);

#if defined(CONFIG_TS_HAL_INPUT_EDGE) || defined(__DOXYGEN__)
/** [any] 注册边沿通知钩子（ISR 上下文回调上抛；去抖归 hal/input.c）。
 * 须在 ts_drv_gpio_in_init 成功后注册；未就绪返回 -ENODEV。 */
int ts_drv_gpio_in_notify(void (*fn)(void));
#endif

/** [any] PWM 真后端写失败计数（观测面；写函数 void 返回的失败留痕）。 */
uint32_t ts_drv_pwm_err_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_SAFETY_H__ */
