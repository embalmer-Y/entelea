/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ts-core 公共 API（design/LLD-ts-core.md）：初始化编排 / 事件总线 / 单调时间 / 看门狗框架。
 * 合同关联：合同 6（初始化顺序固定）、合同 9（唯一时间源）、合同 4（WDT 可定位）。
 */
#ifndef TS_CORE_H__
#define TS_CORE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/err.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 单调时间（LLD-ts-core §3）------------------------------------------ */

typedef struct {
	uint64_t (*now_ms)(void);
} ts_time_source_t;

/** [any] 唯一时间源（k_uptime_get 封装）；测试构建可绑定虚拟时钟。 */
uint64_t ts_time_ms(void);

/** [thread] 墙钟（数据字段专用：遥测/审计时间戳，DR-08）——仅影响数据字段，
 * 控制路径一律用 ts_time_ms（合同 9 确定性；sys 命令 set-time 落点）。 */
void ts_time_wall_set(uint64_t epoch_ms);

/** [any] 墙钟读取（未设置返回 0）。 */
uint64_t ts_time_wall_ms(void);

/** 仅 CONFIG_TS_TEST 构建：L4 虚拟时钟注入（NULL 恢复真实源）。 */
#ifdef CONFIG_TS_TEST
void ts_time_test_bind(const ts_time_source_t *src);
#else
static inline void ts_time_test_bind(const ts_time_source_t *src)
{
	ARG_UNUSED(src);
}
#endif

/* ---- 事件总线（LLD-ts-core §4）------------------------------------------ */

typedef enum {
	TS_EVT_BOOT_STEP,
	TS_EVT_BOOT_DONE,
	TS_EVT_ESTOP, /* 事后补发（合同5）；estop 动作本身不经总线 */
	TS_EVT_NET_LINK_UP,
	TS_EVT_NET_LINK_DOWN,
	TS_EVT_SAFE_STATE_CHANGED, /* payload: 通道 uid + 新态 */
	TS_EVT_APP_LOADED,
	TS_EVT_APP_UNLOADED,
	TS_EVT_APP_QUARANTINED,
	TS_EVT_PERIPH_ATTACH,
	TS_EVT_PERIPH_DETACH,
	TS_EVT_INPUT_CHANGED, /* ts-hal input monitor（DR-02） */
	TS_EVT_WDT_WARN,
	TS_EVT_POWER_BUDGET,
	TS_EVT_PERM_DENIED, /* 合同10 越权留痕 */
	TS_EVT_ARB_CHANGED, /* 控制源仲裁席变更（CG3 · 合同 11：old/owner/t——切换零输出） */
	TS_EVT_RESOURCE_WRITTEN, /* 宿主资源推送完成（CG5-B · 方案 A：sha256 验证通过留痕） */
	TS_EVT_TRUST_UPDATED, /* 信任根表更新（EC2 · Q-43④：ver 严格 N+1 链——安全类留痕） */
	TS_EVT_PENDING_INSTALL, /* 安装闸挂起（F3 · DEC-62①：activate 过闸待本地确认） */
	TS_EVT_INSTALL_CONFIRMED, /* 安装闸本地确认 → meta 已切换（待复位装载） */
	TS_EVT_INSTALL_REJECTED, /* 安装闸本地拒绝（槽保留可重推） */
	TS_EVT_INSTALL_TIMEOUT, /* 安装闸 60s fail-closed 超时撤销（槽保留可重推） */
	TS_EVT_ID_COUNT,
} ts_evt_id_t;

typedef struct {
	ts_evt_id_t id;
	uint64_t t_ms;
	const void *data;
	size_t len;
} ts_evt_t;

typedef void (*ts_evt_cb_t)(const ts_evt_t *evt, void *user);

/* 事件 payload 类型（发布方模块集中定义，重放解码用） */
typedef struct {
	uint8_t idx;
	int32_t result;
	const char *name;
} ts_boot_step_evt_t; /* TS_EVT_BOOT_STEP */

/* TS_EVT_TRUST_UPDATED payload（EC2 · Q-43④：新版本号——留痕/订阅外发） */
typedef struct {
	uint32_t ver;
	uint8_t n_keys; /* 更新后开发者键数 */
} ts_trust_evt_t;

/* TS_EVT_PENDING_INSTALL / INSTALL_{CONFIRMED,REJECTED,TIMEOUT} payload
 *（F3 · DEC-62①——slot 绝对槽号 + entry 实例位；app_id 经 pending_summary 取） */
typedef struct {
	uint8_t slot;
	uint8_t entry;
} ts_pending_evt_t;

/** [thread] 仅 init 期注册；容量 CONFIG_TS_CORE_MAX_SUBS（DEC-27: 4），满 = TS_E_NOMEM。 */
ts_res_t ts_evt_subscribe(ts_evt_id_t id, ts_evt_cb_t fn, void *user);

/** [thread] 同步分发（注册表静态顺序）；[ISR] 投递到深度 CONFIG_TS_CORE_EVT_QUEUE_DEPTH
 * （DEC-27: 16）的队列由 sysworkq 分发。ISR 投递的 data 必须指向静态存储。 */
void ts_evt_publish(const ts_evt_t *evt);

/* ---- 看门狗框架（LLD-ts-core §5）---------------------------------------- */

typedef enum {
	TS_WDT_NET,
	TS_WDT_APPMGR,
	TS_WDT_SYWORK,
	TS_WDT_COUNT,
} ts_wdt_src_t;

/** [any] 巡检周期 = min(periods)/2（观测/测试用）。 */
uint32_t ts_wdt_patrol_period_ms(void);

typedef struct {
	ts_wdt_src_t src;
	uint32_t last_feed;
} ts_wdt_warn_evt_t; /* TS_EVT_WDT_WARN payload */

/* TS_EVT_PERIPH_ATTACH/DETACH payload（M3b，LLD-ts-periph §3） */
typedef struct {
	const char *uid; /* 逻辑名（注册期冻结） */
	uint8_t kind;    /* ts_periph_kind_t 值（core 不引 periph 头——u8 传输） */
} ts_periph_evt_t;

/* TS_EVT_POWER_BUDGET payload（M3b，LLD-ts-power §3：超预算拒绝外发） */
typedef struct {
	uint32_t requested_ma;
	uint32_t used_ma;
	uint32_t budget_ma;
} ts_pwr_budget_evt_t;

/** [thread] init 期注册喂狗源（period_ms 出处 DEC-22/27 语义：各子系统周期）。 */
ts_res_t ts_wdt_register(ts_wdt_src_t src, uint32_t period_ms);

/** [any] 原子更新 last_feed。 */
void ts_wdt_feed(ts_wdt_src_t src);

/** [any] 声明式阻塞豁免窗（R1·DEC-64①）：源即将进入**有预算的合法阻塞操作**
 *  （net open 体等）。窗内巡检不判该源逾期；超窗恢复判逾（真卡死仍 fail-safe）；
 *  feed 清除。未注册/未激活源 no-op（grace 不激活源——激活 = feed 专属）。 */
void ts_wdt_grace(ts_wdt_src_t src, uint32_t budget_ms);

/** [thread] 去激活喂狗源（DEC-48① 对称面：APPMGR 随 APP 停止去激活——
 *  停后无人喂 = 预期态不判逾期；下次 feed 重新激活）。单元 F。 */
void ts_wdt_deactivate(ts_wdt_src_t src);

/** [thread] 启动巡检（sysworkq 周期项，周期 = min(periods)/2）；硬件 WDT 超时 =
 * min(2×max(periods), 10s)（DEC-22/DEC-27）。逾期 → TS_EVT_WDT_WARN → system_fail。 */
ts_res_t ts_wdt_start(void);

/* ---- 初始化编排（LLD-ts-core §2）---------------------------------------- */

typedef ts_res_t (*ts_boot_step_fn)(void);
typedef struct {
	const char *name;
	ts_boot_step_fn fn;
} ts_boot_step_t;

/*
 * 顺序 = HLD §4.4：estop GPIO → 全通道 SAFE_POWERON → WDT 启动 → core →
 * periph/hal → 存储 → net → appmgr → 运行态。顺序是规格，数组只允许尾部追加
 * （M2/M3 各里程碑按序补 periph/hal/store/net/appmgr 步骤），不可调换既有次序。
 * 来源: 结构性数值（步骤计数随里程碑追加更新，HLD §4.4）
 */
#if defined(CONFIG_TS_NET) && defined(CONFIG_TS_APP_WAMR)
#define TS_BOOT_STEP_COUNT_BASE 6 /* M1 四步 + net_init（M3a.2）+ app_load（M2b.2 步骤 8）；来源: HLD §4.4 */
#elif defined(CONFIG_TS_NET)
#define TS_BOOT_STEP_COUNT_BASE 5 /* M1 四步 + net_init（M3a.2 尾部追加）；来源: HLD §4.4 */
#elif defined(CONFIG_TS_APP_WAMR)
#define TS_BOOT_STEP_COUNT_BASE 5 /* M1 四步 + app_load（M2b.2 步骤 8）；来源: HLD §4.4 */
#else
#define TS_BOOT_STEP_COUNT_BASE 4 /* M1: estop/poweron/wdt/core；来源: 结构性数值（HLD §4.4） */
#endif
#if defined(CONFIG_TS_STORE_AUDIT)
/* DEC-52①（V2-2 尾部追加）：审计 flash 环恢复 + boot 报告（最近 8 条） */
#define TS_BOOT_STEP_COUNT (TS_BOOT_STEP_COUNT_BASE + 1)
#else
#define TS_BOOT_STEP_COUNT TS_BOOT_STEP_COUNT_BASE
#endif
extern const ts_boot_step_t ts_boot_steps[TS_BOOT_STEP_COUNT];

/** [thread] 固定顺序执行；任一步失败 → ts_safety_system_fail(TS_FAIL_BOOT(idx))
 * 后受控停机（喂狗停止 → 硬 WDT 兜底复位；noinit 留痕经 ts-store，M2 接入）。 */
FUNC_NORETURN void ts_core_boot(void);

/* ---- 测试钩子（仅 CONFIG_TS_TEST，L4 同步驱动用）------------------------- */
#ifdef CONFIG_TS_TEST
/** 排空 ISR 投递队列并同步分发（测试内同步观测事件）。 */
void ts_evt_poll_drain(void);
/** 单次巡检判定（不等待真实周期），返回逾期源（无则 TS_WDT_COUNT）。 */
ts_wdt_src_t ts_wdt_patrol_once(void);
/** 豁免窗活性查询（窗在且未过期）——net open 接线用例消费。 */
bool ts_wdt_grace_active(ts_wdt_src_t src);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TS_CORE_H__ */
