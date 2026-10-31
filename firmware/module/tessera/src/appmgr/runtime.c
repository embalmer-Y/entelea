/* SPDX-License-Identifier: Apache-2.0 */
/* APP 运行时宿主（LLD-ts-appmgr §4/§5；DEC-43 方案 A：每 APP 一个框架线程）。
 * V0.4.0 M7（Q-32③/DEC-53）：rt[TS_APP_MAX_INSTANCES] 数组——每 entry 一
 * 运行时槽（数值 app_id = entry+1：boot/审计归因/natives ctx 同源），线程/
 * 静态栈/邮箱/模块对象 per rt 并行；WAMR 共享池预算不变（实例线性内存按需
 * 页，manifest.heap_kb 申报制随 M8）。事件串行化（DR-14）：tick/app_evt 均
 * 在 APP 线程内执行；外部事件（input monitor）**广播**至全部运行中实例。
 * 停止语义（DR-14）：停投递 → join（2s 超时弃管升级 WDT）→ 回收（env/实例/
 * 邮箱）；模块对象 rt 级持有不卸载（WAMR 平台怪癖规避——换包重启路径）。
 * 健康探针：health_ping 周期 1000ms，连续 3 败 → health_fail_id(app_id)
 * （per-entry 回滚链，M7 分账）。WDT：APPMGR 单源**群组喂狗**（Q-32④——
 * 任一 APP 线程喂即活；全卡死才升级；单实例卡死由健康探针回滚兜底）。
 * WAMR 生命周期：进程级单次 init/注册（平台层 init/destroy 不对称）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <ts/appmgr.h>
#include <ts/core.h> /* ts_time_ms：合同 9 时基（L5 禁 uptime） */
#include <ts/hal.h>
#include <wasm_export.h>
#if defined(CONFIG_TS_APP_PSRAM_HEAP)
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif

#define APP_STOP_JOIN_MS 2000 /* DEC-27 #15 */
#define APP_WASM_STACK   4096 /* wasm 应用栈（manifest.mem.stack 上限 8KB 前
			       * 的 V1 缺省；随 manifest 实装读入） */
#define APP_WASM_HEAP    8192
#ifdef CONFIG_TS_APP_THREADS
#define TS_APP_THR_SLOTS 3U /* manifest threads ≤4 → 额外线程 ≤3（Q-33 域） */
#define THR_NAME_MAX     24U

/* worker 线程上下文（per rt per slot；k_thread 对象 + 退出值回传位） */
struct ts_thr_ctx {
	uint8_t used; /* spawn 起 join 止 */
	wasm_exec_env_t env;
	struct k_thread thread;
	int32_t ret;
};
#endif

extern bool ts_app_natives_register(void);

struct ts_app_rt {
	bool used;
	atomic_t running;
	uint16_t app_id;
	ts_ctx_t ctx; /* hal 绑定（表已拷贝进绑定层，栈上表即可） */
	wasm_module_t mod;
	wasm_module_inst_t inst;
	wasm_exec_env_t env;
	wasm_function_inst_t fn_init, fn_tick, fn_evt, fn_health;
	struct k_thread thread;
	char app_id_str[64]; /* manifest 串身份（V0.4.0 M9：kv 哈希源，DR-15） */
	/* 观测计数（stats） */
	uint32_t evt_seen, tick_count, health_fails, mb_dropped, init_res;
	int32_t last_evt;
	bool health_failed;
#ifdef CONFIG_TS_APP_THREADS
	/* DEC-55（Q-37 A）：宿主侧线程编排——一次性 worker 模型。
	 * threads_max = manifest threads（总并发含主；boot 装载期注入，
	 * 0/1 = 单线程 APP spawn 即拒）；槽位 = threads_max-1 个
	 * （wasm 侧句柄 = 槽位 +1；join 后槽复用）。
	 * 线程载体 = 我方 k_thread（app 主线程同款机制）——**不经 WAMR
	 * zephyr 平台层 os_thread 族**（2.4.5 实证缺陷：join 对已退线程
	 * 恒 0 空返、退出值从不回传、thread_obj 回收断言竞态——sim 三态
	 * 复现）；WAMR 侧仅用公共 API spawn_exec_env/destroy（纯对象
	 * 创建/回收，无线程参与）。 */
	uint8_t threads_max;
	uint8_t threads_active;
	struct ts_thr_ctx thr[TS_APP_THR_SLOTS];
#endif
};

static struct ts_app_rt rt[CONFIG_TS_APP_MAX_INSTANCES];
static K_THREAD_STACK_ARRAY_DEFINE(app_stacks, CONFIG_TS_APP_MAX_INSTANCES,
				   CONFIG_TS_APP_THREAD_STACK);

/* 模块缓存（WAMR 平台怪癖规避——教训 12：同进程 load→unload→reload 与
 * 二次 load 均失败；唯一可靠形态 = 同字节流复用已 load 的模块对象，永不
 * 卸载。数组化（M7）：多 APP 各自模块共存；条目按字节流身份匹配，容量
 * = N+1（超出 = 不缓存直载，生产换包走重启路径）。**全局层**（非 rt 级）：
 * 缓存须挺过 rt 复位（测试 test_reset memset rt 的既有形态）。 */
static struct {
	const uint8_t *src;
	uint32_t len;
	wasm_module_t mod;
} mod_cache[CONFIG_TS_APP_MAX_INSTANCES + 1];
static size_t mod_cache_n;

ts_ctx_t ts_app_rt_ctx(struct ts_app_rt *r)
{
	return r->ctx; /* natives ctx_of：exec_env user_data = rt 槽位指针 */
}

const char *ts_app_rt_idstr(struct ts_app_rt *r)
{
	return r->app_id_str; /* V0.4.0 M9：kv 哈希源（boot 装载期注入） */
}

void ts_appmgr_app_set_idstr(uint16_t app_id, const char *idstr)
{
	/* boot 装载期注入 manifest 串身份（entry 对齐：app_id = entry+1） */
	if (app_id == 0 || app_id > CONFIG_TS_APP_MAX_INSTANCES ||
	    idstr == NULL) {
		return;
	}
	strncpy(rt[app_id - 1].app_id_str, idstr,
		sizeof(rt[app_id - 1].app_id_str) - 1);
	rt[app_id - 1].app_id_str[sizeof(rt[app_id - 1].app_id_str) - 1] = '\0';
}

#ifdef CONFIG_TS_APP_THREADS
void ts_appmgr_app_set_threads(uint16_t app_id, uint8_t threads)
{
	/* boot 装载期注入 manifest threads（DEC-55：总并发含主；1-4 域已在
	 * slot.c manifest 走查期校验——此处收口为 rt 上限）。 */
	if (app_id == 0 || app_id > CONFIG_TS_APP_MAX_INSTANCES) {
		return;
	}
	rt[app_id - 1].threads_max = (threads > 4) ? 4 : threads;
}

/* ---- 宿主侧线程编排（DEC-55/Q-37 A）------------------------------------
 * WAMR 侧仅用公共 API：wasm_runtime_spawn_exec_env（创建共享内存兄弟
 * 实例 + 独立 exec_env——纯对象创建，无线程参与）/ destroy_spawned_exec_env。
 * 线程载体 = 我方 k_thread（app 主线程同款已证机制）——**不经 WAMR zephyr
 * 平台层 os_thread 族**（2.4.5 实证缺陷：join 对已退线程恒 0 空返且退出值
 * 从不回传、thread_obj 回收断言竞态——sim 三态复现；本段替其语义）。
 * ctx 注入（user_data）与每线程指令预算（DEC-55④ L2 硬上界）都在 wasm
 * 调用前由宿主设定（绕开 lib-pthread 计量初值 0 的上游死结）。worker =
 * 一次性任务：入口返回即线程结束（跨线程数据 = 共享线性内存 + 原子内建）。 */
struct ts_thr_arg {
	struct ts_app_rt *rt;
	uint8_t slot;
	char name[THR_NAME_MAX];
	uint32_t arg;
};
/* per-rt per-slot 静态参数区（k_malloc 不入模块依赖——基线构建零新增
 * Kconfig 面；槽生命周期 = spawn→join，join 即线程已终止 → 复用零竞态） */
static struct ts_thr_arg thr_args[CONFIG_TS_APP_MAX_INSTANCES][TS_APP_THR_SLOTS];
static K_THREAD_STACK_ARRAY_DEFINE(
	thr_stacks, CONFIG_TS_APP_MAX_INSTANCES * TS_APP_THR_SLOTS,
	CONFIG_TS_APP_THREAD_WSTACK);

static void thr_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct ts_thr_arg *a = p1;
	struct ts_thr_ctx *c = &a->rt->thr[a->slot];
	wasm_exec_env_t env = c->env;
	int32_t rc = (int32_t)TS_E_NOTFOUND;

	wasm_runtime_set_user_data(env, a->rt); /* natives ctx 注入（跨线程同 rt） */
	wasm_runtime_set_instruction_count_limit(env,
						 CONFIG_TS_APP_INSTR_THREAD);
	wasm_function_inst_t fn = wasm_runtime_lookup_function(
		wasm_runtime_get_module_inst(env), a->name);

	if (fn != NULL) {
		uint32_t argv[1] = {a->arg};

		if (wasm_runtime_call_wasm(env, fn, 1, argv)) {
			rc = (int32_t)argv[0];
		} else {
			/* 失败可见性（军规 7）：worker 异常文本留痕 */
			const char *exc = wasm_runtime_get_exception(
				wasm_runtime_get_module_inst(env));

			if (exc != NULL) {
				printk("[appmgr] app%u thread '%s' exception: %s\n",
				       (unsigned)a->rt->app_id, a->name, exc);
			}
			rc = (int32_t)TS_E_IO;
		}
	} else {
		printk("[appmgr] app%u thread '%s' entry not found\n",
		       (unsigned)a->rt->app_id, a->name);
	}
	c->ret = rc; /* join 回传（k_thread_join 后读——线程退出即写毕可见） */
	wasm_runtime_destroy_spawned_exec_env(env);
}

int ts_app_rt_thread_start(struct ts_app_rt *r, const char *name, uint32_t arg,
			   uint32_t *handle)
{
	if (r == NULL || name == NULL || handle == NULL) {
		return TS_E_PARAM;
	}
	if (r->threads_max < 2) {
		return TS_E_STATE; /* 未申报 threads（DEC-31 单线程语义，fail-closed） */
	}
	if (r->threads_active >= r->threads_max - 1 ||
	    r->threads_active >= TS_APP_THR_SLOTS) {
		return TS_E_RANGE; /* 并发上限（manifest threads 域） */
	}
	size_t n = strlen(name);

	if (n == 0 || n >= THR_NAME_MAX) {
		return TS_E_PARAM;
	}
	int slot = -1;

	for (size_t i = 0; i < TS_APP_THR_SLOTS; i++) {
		if (r->thr[i].used == 0) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0) {
		return TS_E_RANGE;
	}
	wasm_exec_env_t env = wasm_runtime_spawn_exec_env(r->env);

	if (env == NULL) {
		return TS_E_NOMEM;
	}
	struct ts_thr_arg *a = &thr_args[r - rt][slot];

	a->rt = r;
	a->slot = (uint8_t)slot;
	memcpy(a->name, name, n + 1);
	a->arg = arg;
	r->thr[slot].env = env;
	r->thr[slot].ret = 0;
	r->thr[slot].used = 1;
	k_tid_t tid = k_thread_create(
		&r->thr[slot].thread,
		thr_stacks[(r - rt) * TS_APP_THR_SLOTS + slot],
		K_THREAD_STACK_SIZEOF(thr_stacks[0]), thr_entry, a, NULL, NULL,
		CONFIG_TS_APP_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (tid == NULL) {
		wasm_runtime_destroy_spawned_exec_env(env);
		r->thr[slot].used = 0;
		return TS_E_NOMEM;
	}
	r->threads_active++;
	*handle = (uint32_t)slot + 1; /* wasm 侧句柄（1 起） */
	return TS_OK;
}

int ts_app_rt_thread_join(struct ts_app_rt *r, uint32_t handle)
{
	if (r == NULL || handle == 0 || handle > TS_APP_THR_SLOTS) {
		return TS_E_PARAM;
	}
	size_t slot = handle - 1;

	if (r->thr[slot].used == 0) {
		return TS_E_STATE; /* 未 spawn/已 join（幂等拒绝） */
	}
	/* join 有界：worker 被 L2 计量硬封顶（DEC-55④）+ natives 全非阻塞。
	 * APP 停止路径同此（stop 先 join 全部 worker 再回收主实例——兄弟实例
	 * 共享主线性内存，序不可反）。 */
	if (k_thread_join(&r->thr[slot].thread, K_FOREVER) != 0) {
		return TS_E_IO;
	}
	r->thr[slot].used = 0;
	r->threads_active--;
	return r->thr[slot].ret; /* worker 返回值（>=0）或 ts_res_t */
}
#endif /* CONFIG_TS_APP_THREADS */

/* 每 rt 独立 mailbox（DR-14 深度 DEC-27 #15: 8，满丢最旧 + 计数） */
static uint32_t mb_bufs[CONFIG_TS_APP_MAX_INSTANCES][CONFIG_TS_APP_MBOX_DEPTH];
static struct k_msgq mbs[CONFIG_TS_APP_MAX_INSTANCES];
static bool mbs_inited;

static void mbs_init_once(void)
{
	if (!mbs_inited) {
		for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
			k_msgq_init(&mbs[i], (char *)mb_bufs[i], sizeof(uint32_t),
				    CONFIG_TS_APP_MBOX_DEPTH);
		}
		mbs_inited = true;
	}
}

/* ---- APP 线程主循环 -------------------------------------------------------- */

static bool call1(struct ts_app_rt *r, wasm_function_inst_t fn, uint32_t arg,
		  uint32_t *ret)
{
	uint32_t argv[1] = {arg};

	/* DEC-48③（Q-28③A）：每次调用前设指令配额——超限抛
	 * "instruction limit exceeded" → 返回 false → 既有健康失败/回滚路径
	 * （无 abort、无持锁死锁面）。tick/evt/health 共用 TICK 预算。 */
	wasm_runtime_set_instruction_count_limit(r->env, CONFIG_TS_APP_INSTR_TICK);
	if (!wasm_runtime_call_wasm(r->env, fn, 1, argv)) {
		/* FIX-2：WAMR 实例异常位跨调用存活（wasm_call_function 以
		 * 异常位判成败）——一陷毒化后续全部调用（health 连锁败）。
		 * 失败即清，下次调用不受前次残留影响。 */
		wasm_runtime_clear_exception(r->inst);
		return false;
	}
	if (ret != NULL) {
		*ret = argv[0];
	}
	return true;
}

static void health_probe(struct ts_app_rt *r)
{
	uint32_t argv[1] = {r->app_id};
	bool ok;

	/* 健康语义：调用成功且返回 0（异常或非零 = 失败一次）。
	 * FIX-2 三修：显式设 TICK 配额（原继承上次调用余量）+ argc=1 带 ctx
	 * （argc=0 对带参 health 必陷 "invalid argument count"；WAMR 对无参
	 * 形态钳制不扰）+ 失败清残留异常（call1 同律）。 */
	wasm_runtime_set_instruction_count_limit(r->env, CONFIG_TS_APP_INSTR_TICK);
	ok = wasm_runtime_call_wasm(r->env, r->fn_health, 1, argv);
	if (!ok || argv[0] != 0) { /* 返回值经 argv[0] 回传（WAMR 约定） */
		wasm_runtime_clear_exception(r->inst);
		r->health_fails++;
		if (r->health_fails >= CONFIG_TS_APP_HEALTH_FAILS) {
			r->health_failed = true;
			atomic_set(&r->running, 0);
			/* M7：per-entry 回滚分账（Q-32⑤/DEC-53） */
			(void)ts_appmgr_health_fail_id(r->app_id);
		}
		return;
	}
	r->health_fails = 0;
}

static void app_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct ts_app_rt *r = p1;
	struct k_msgq *mb = &mbs[r - rt];

	ts_wdt_feed(TS_WDT_APPMGR); /* DEC-48②：线程入口激活喂狗源（群组） */
	if (r->fn_init != NULL) {
		uint32_t ret = 0;

		wasm_runtime_set_instruction_count_limit(r->env,
							 CONFIG_TS_APP_INSTR_INIT);
		if (!call1(r, r->fn_init, r->app_id, &ret)) {
			/* 失败可见性（军规 7 如实上报）：WAMR 异常文本留痕 */
			const char *exc = wasm_runtime_get_exception(r->inst);

			if (exc != NULL) {
				printk("[appmgr] app%u init exception: %s\n",
				       (unsigned)r->app_id, exc);
			}
			r->init_res = (uint32_t)TS_E_IO; /* init 异常 = 启动失败 */
			atomic_set(&r->running, 0);
			return;
		}
		r->init_res = ret;
	}
	uint32_t last_tick = (uint32_t)ts_time_ms();
	uint32_t last_health = last_tick;
	uint32_t v;

	while (atomic_get(&r->running) != 0) {
		ts_wdt_feed(TS_WDT_APPMGR); /* 群组喂狗（≤20ms 轮询上界） */
		if (k_msgq_get(mb, &v, K_MSEC(20)) == 0) {
			r->evt_seen++;
			r->last_evt = (int32_t)v;
			if (r->fn_evt != NULL && !call1(r, r->fn_evt, v, NULL)) {
				r->health_fails++; /* evt 异常计入健康 */
			}
		}
		uint32_t now = (uint32_t)ts_time_ms();

		if (r->fn_tick != NULL &&
		    (now - last_tick) >= CONFIG_TS_APP_TICK_MS) {
			last_tick = now;
			r->tick_count++;
			/* FIX-2：tick 经 call1 带 ctx（约定 app_tick(int ctx)
			 * 与 init 同型；argc=1 ≥ 任意 ≤1 参 cell 数——原 call0
			 * argc=0 对带参 tick 必陷 "invalid argument count 0,
			 * must be no smaller than 1"（P4 探针复位循环根因）；
			 * 无参 fixture tick（app_tick(void)）被 WAMR 钳制不扰） */
			if (!call1(r, r->fn_tick, r->app_id, NULL)) {
				r->health_fails++;
			}
		}
		if ((now - last_health) >= CONFIG_TS_APP_HEALTH_MS) {
			last_health = now;
			health_probe(r);
		}
	}
}

/* ---- 公共 API -------------------------------------------------------------- */

/* G3（单元 F）→ M7 广播：input monitor → 全部运行中实例 mailbox。
 * 订阅 TS_EVT_INPUT_CHANGED（一次性——evt 总线无退订 API，cb 内按
 * rt.used/running 门控）；payload 编码进 32 位 mailbox 槽：
 * (inst << 16) | (new_mv & 0xFFFF)——旧值/阈值语义归 APP。
 * 回调在发布者上下文（sysworkq 输入轮询）执行；k_msgq_put(K_NO_WAIT)
 * 线程安全，满 = 丢最旧 + 计数（DR-14）。 */
static void input_evt_cb(const ts_evt_t *evt, void *user)
{
	ARG_UNUSED(user);
	if (evt->data == NULL || evt->len < sizeof(struct ts_input_evt)) {
		return;
	}
	const struct ts_input_evt *p = evt->data;
	const uint32_t payload = ((uint32_t)(p->inst & 0xFFFF) << 16) |
				 ((uint32_t)p->new_mv & 0xFFFF);

	(void)ts_appmgr_app_evt_bcast(payload);
}

static void input_route_subscribe(void)
{
	static bool done;

	if (!done && ts_evt_subscribe(TS_EVT_INPUT_CHANGED, input_evt_cb,
				      NULL) == TS_OK) {
		done = true;
	}
}

#ifdef CONFIG_XTENSA
/* DAV1 诊断（B2 后续批）：APPMGR 软看门狗逾期警告时 dump 冻结线程状态与
 * 栈内文本域字（返回地址候选——离线 addr2line 定位冻结点）。一次 dump 防
 * 洪泛（M7：扫全部 rt，输出第一个非运行态卡死者）。 */
static void wdt_warn_diag_cb(const ts_evt_t *e, void *user)
{
	ARG_UNUSED(user);
	if (e->data == NULL || e->len < sizeof(ts_wdt_warn_evt_t)) {
		return;
	}
	const ts_wdt_warn_evt_t *w = e->data;

	if (w->src != TS_WDT_APPMGR) {
		return;
	}
	static bool dumped;

	if (dumped) {
		return;
	}
	dumped = true;
	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		struct ts_app_rt *r = &rt[i];

		if (!r->used) {
			continue;
		}
		char sb[48];

		printk("[appmgr] WDT-WARN diag: app%u state=%s prio=%d\n",
		       (unsigned)r->app_id,
		       k_thread_state_str((k_tid_t)&r->thread, sb, sizeof(sb)),
		       (int)r->thread.base.prio);
		/* 文本域字扫描（esp32s3：IRAM 0x40xxxxxx / IROM 0x42xxxxxx）——
		 * 返回地址候选按栈顶近端优先输出（最近调用层在前），上限 40 条。 */
		const uint32_t *st = (const uint32_t *)r->thread.stack_info.start;
		const size_t words = r->thread.stack_info.size / sizeof(uint32_t);
		int shown = 0;

		for (size_t j = words; j > 0 && shown < 40; j--) {
			uint32_t v = st[j - 1];

			if ((v & 0xFF000000U) == 0x40000000U ||
			    (v & 0xFF000000U) == 0x42000000U) {
				printk("[appmgr] wf[%02d] +%04x %08x\n", shown++,
				       (unsigned)((words - j) * 4U), v);
			}
		}
	}
}
#endif

static struct ts_app_rt *rt_by_id(uint16_t app_id)
{
	if (app_id == 0 || app_id > CONFIG_TS_APP_MAX_INSTANCES) {
		return NULL;
	}
	return &rt[app_id - 1];
}

ts_res_t ts_appmgr_app_start(uint16_t app_id, const uint8_t *wasm,
			     uint32_t wasm_len, const char *caps)
{
	char err[128];

	if (wasm == NULL || wasm_len == 0 || caps == NULL) {
		return TS_E_PARAM;
	}
	struct ts_app_rt *r = rt_by_id(app_id);

	if (r == NULL) {
		return TS_E_PARAM; /* app_id 超界（entry 对齐：1..N） */
	}
	if (r->used) {
		return TS_E_STATE;
	}
	mbs_init_once();
	/* WAMR 进程级初始化 + natives 全局注册（一次）。
	 * 实例堆 = 显式池（DEC-27 #10 池模式；板级六修正：池语义须经
	 * wasm_runtime_full_init 显式注入——共享池预算跨实例分摊，Q-32③）：
	 *  - PSRAM 配置（CONFIG_TS_APP_PSRAM_HEAP，HLD §4.6 分层纪律）=
	 *    共享多堆 SMH_REG_ATTR_EXTERNAL 分配（esp32s3 PSRAM 官方机制）；
	 *  - 其余（native_sim/CI/未挂 PSRAM 板）= 内部 SRAM 静态池。 */
	static bool wamr_up;

	if (!wamr_up) {
#if defined(CONFIG_TS_APP_PSRAM_HEAP)
		static void *wamr_heap_buf;

		if (wamr_heap_buf == NULL) {
			wamr_heap_buf = shared_multi_heap_alloc(
				SMH_REG_ATTR_EXTERNAL, CONFIG_TS_APP_WAMR_HEAP);
		}
		if (wamr_heap_buf == NULL) {
			printk("[appmgr] wamr psram heap alloc %u failed\n",
			       CONFIG_TS_APP_WAMR_HEAP);
			return TS_E_NOMEM;
		}
#else
		static uint8_t wamr_heap_buf[CONFIG_TS_APP_WAMR_HEAP] __aligned(8);
#endif
		RuntimeInitArgs args = {0};

		args.mem_alloc_type = Alloc_With_Pool;
		args.mem_alloc_option.pool.heap_buf = wamr_heap_buf;
		args.mem_alloc_option.pool.heap_size = CONFIG_TS_APP_WAMR_HEAP;
		if (!wasm_runtime_full_init(&args)) {
#if defined(CONFIG_TS_APP_PSRAM_HEAP)
			shared_multi_heap_free(wamr_heap_buf);
			wamr_heap_buf = NULL;
#endif
			return TS_E_IO;
		}
		printk("[appmgr] wamr pool heap: buf=%p size=%u (%s)\n",
		       args.mem_alloc_option.pool.heap_buf,
		       CONFIG_TS_APP_WAMR_HEAP,
		       IS_ENABLED(CONFIG_TS_APP_PSRAM_HEAP) ? "psram/smh" : "sram/static");
		wamr_up = true;
	}
	if (!ts_app_natives_register()) {
		return TS_E_IO;
	}
	/* 权限表 → hal ctx 绑定（表拷贝入绑定层；防伪造边界）。
	 * caps 可为 ';' 分隔多条（boot 组合 manifest caps 数组；ts_perm_parse
	 * 语义 = 位图或合并） */
	ts_perm_table_t table;

	ts_perm_table_init(&table);
	if (caps[0] != '\0') {
		const char *p = caps;

		while (*p != '\0') {
			const char *seg = strchr(p, ';');
			size_t sl = (seg != NULL) ? (size_t)(seg - p) : strlen(p);
			char one[48];

			if (sl == 0 || sl >= sizeof(one)) {
				return TS_E_PARAM;
			}
			memcpy(one, p, sl);
			one[sl] = '\0';
			if (ts_perm_parse(one, &table) != TS_OK) {
				return TS_E_PARAM;
			}
			if (seg == NULL) {
				break; /* 末段（无 ';'）——防越过 NUL 越界 */
			}
			p = seg + 1;
		}
	}
#ifdef CONFIG_TS_TEST
	/* V0.4.0 M2（IR-16）：TEST 语义自动授予 log:write:0——历史测试根签名包
	 * （nv1/demos 全系）manifest 无 log 声明的延续性；生产包须显式声明
	 * （fail-closed），spec 模板/skills 随 Agent 面同步。 */
	(void)ts_perm_parse("log:write:0", &table);
#endif
	if (ts_hal_bind_context(&r->ctx, app_id, &table) != TS_OK) {
		return TS_E_NOMEM;
	}
	/* 模块经全局缓存（见 mod_cache 注释）：同字节流复用；未命中 = load
	 * 并入缓存（容量外 = 直载不入缓存）。生产换包 = 重启路径（怪癖规避）。 */
	r->mod = NULL;
	for (size_t i = 0; i < mod_cache_n; i++) {
		if (mod_cache[i].len == wasm_len &&
		    memcmp(mod_cache[i].src, wasm, wasm_len) == 0) {
			r->mod = mod_cache[i].mod;
			break;
		}
	}
	if (r->mod == NULL) {
		r->mod = wasm_runtime_load((uint8_t *)wasm, wasm_len, err, sizeof(err));
		if (r->mod == NULL) {
			printk("[appmgr] load fail: %s (len=%u)\n", err, wasm_len);
			ts_hal_unbind_context(&r->ctx);
			return TS_E_IO;
		}
		if (mod_cache_n < sizeof(mod_cache) / sizeof(mod_cache[0])) {
			mod_cache[mod_cache_n].src = wasm;
			mod_cache[mod_cache_n].len = wasm_len;
			mod_cache[mod_cache_n].mod = r->mod;
			mod_cache_n++;
		}
	}
	r->inst = wasm_runtime_instantiate(r->mod, APP_WASM_STACK, APP_WASM_HEAP,
					   err, sizeof(err));
	if (r->inst == NULL) {
		printk("[appmgr] inst fail: %s\n", err);
		/* 模块全局缓存持有（怪癖规避——不卸载） */
		ts_hal_unbind_context(&r->ctx);
		return TS_E_IO;
	}
	r->env = wasm_runtime_create_exec_env(r->inst, APP_WASM_STACK);
	if (r->env == NULL) {
		wasm_runtime_deinstantiate(r->inst);
		/* 模块全局缓存持有（怪癖规避——不卸载） */
		ts_hal_unbind_context(&r->ctx);
		return TS_E_NOMEM;
	}
	wasm_runtime_set_user_data(r->env, r);

	r->fn_health = wasm_runtime_lookup_function(r->inst, "health_ping");
	if (r->fn_health == NULL) {
		/* manifest v1 必选导出（缺 = 包非法） */
		wasm_runtime_destroy_exec_env(r->env);
		wasm_runtime_deinstantiate(r->inst);
		/* 模块全局缓存持有（怪癖规避——不卸载） */
		ts_hal_unbind_context(&r->ctx);
		return TS_E_PARAM;
	}
	r->fn_init = wasm_runtime_lookup_function(r->inst, "app_init");
	r->fn_tick = wasm_runtime_lookup_function(r->inst, "app_tick");
	r->fn_evt = wasm_runtime_lookup_function(r->inst, "app_evt");

	k_msgq_purge(&mbs[r - rt]);
	atomic_set(&r->running, 1);
	r->used = true;
	input_route_subscribe(); /* G3：INPUT_CHANGED → 全实例广播 */
#ifdef CONFIG_XTENSA
	(void)ts_evt_subscribe(TS_EVT_WDT_WARN, wdt_warn_diag_cb, NULL);
#endif
	k_tid_t th = k_thread_create(&r->thread,
				     app_stacks[r - rt],
				     K_THREAD_STACK_SIZEOF(app_stacks[0]),
				     app_thread_entry, r, NULL, NULL,
				     CONFIG_TS_APP_THREAD_PRIORITY, 0,
				     K_NO_WAIT);
	if (th == NULL) {
		r->used = false;
		atomic_set(&r->running, 0);
		wasm_runtime_destroy_exec_env(r->env);
		wasm_runtime_deinstantiate(r->inst);
		/* 模块全局缓存持有（怪癖规避——不卸载） */
		ts_hal_unbind_context(&r->ctx);
		return TS_E_NOMEM;
	}
	return TS_OK;
}

ts_res_t ts_appmgr_app_stop_id(uint16_t app_id)
{
	struct ts_app_rt *r = rt_by_id(app_id);

	if (r == NULL || !r->used) {
		return TS_E_STATE;
	}
	atomic_set(&r->running, 0); /* 停止投递；线程排空 mailbox 后自退 */
	if (k_thread_join(&r->thread, K_MSEC(APP_STOP_JOIN_MS)) != 0) {
		/* IR2-06 根治（单元 F）：join 超时 = 线程卡在不可中断点——
		 * **不 k_thread_abort**（持锁点截断 = 永久死锁）。弃管升级：
		 * 线程留原地（同 APPMGR 群组下其余实例照常喂狗——Q-32④）→
		 * 全员卡死才 WDT 升级；本实例由健康探针回滚链处置。资源不
		 * 回收——重启即净；如实登记（E_TIMEOUT）。 */
		printk("[appmgr] app%u stop: thread stuck — abandon to group WDT\n",
		       (unsigned)app_id);
		return TS_E_TIMEOUT;
	}
#ifdef CONFIG_TS_APP_THREADS
	/* DEC-55：先 join 全部 worker 再回收主实例——兄弟实例共享主线性
	 * 内存（共享内存模块），逆序回收 = use-after-free 面。worker 被
	 * L2 计量封顶（终有界）+ natives 非阻塞 → join 有界；未 join 槽
	 * （APP 遗漏）在此强收。 */
	for (size_t i = 0; i < TS_APP_THR_SLOTS; i++) {
		if (r->thr[i].used != 0) {
			(void)ts_app_rt_thread_join(r, (uint32_t)i + 1);
		}
	}
#endif
	wasm_runtime_destroy_exec_env(r->env);
	wasm_runtime_deinstantiate(r->inst);
	/* 模块全局缓存持有（怪癖规避——不卸载） */
	ts_hal_unbind_context(&r->ctx);
	k_msgq_purge(&mbs[r - rt]);
	r->used = false;
	/* 群组喂狗（Q-32④）：全部实例停止后才无人喂 = 预期态才 deactivate */
	bool any = false;

	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		if (rt[i].used) {
			any = true;
			break;
		}
	}
	if (!any) {
		ts_wdt_deactivate(TS_WDT_APPMGR);
	}
	return TS_OK;
}

ts_res_t ts_appmgr_app_stop(void)
{
	return ts_appmgr_app_stop_id(1); /* legacy 单 APP = entry 0 */
}

ts_res_t ts_appmgr_app_evt_id(uint16_t app_id, uint32_t payload)
{
	struct ts_app_rt *r = rt_by_id(app_id);

	if (r == NULL || !r->used || atomic_get(&r->running) == 0) {
		return TS_E_STATE;
	}
	struct k_msgq *mb = &mbs[r - rt];

	if (k_msgq_put(mb, &payload, K_NO_WAIT) != 0) {
		uint32_t drop;

		(void)k_msgq_get(mb, &drop, K_NO_WAIT); /* 满丢最旧（DR-14） */
		r->mb_dropped++;
		(void)k_msgq_put(mb, &payload, K_NO_WAIT);
	}
	return TS_OK;
}

ts_res_t ts_appmgr_app_evt(uint32_t payload)
{
	return ts_appmgr_app_evt_id(1, payload); /* legacy 单 APP = entry 0 */
}

void ts_appmgr_app_evt_bcast(uint32_t payload)
{
	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		(void)ts_appmgr_app_evt_id((uint16_t)(i + 1), payload);
	}
}

/* ---- APP 间消息路由（P1-1 · HLD §3.4）-------------------------------------
 * 每实例定长 FIFO（TS_APPMGR_MSG_SLOTS × TS_APPMGR_MSG_MAX）；满槽 = 发送
 * 方 TS_E_BUSY 背压（不静默丢弃——与 mailbox 满丢最旧语义分立：消息是
 * 点对点语义，mailbox 是广播观测语义）。投递 = 槽入队 + mailbox
 * TS_APP_EVT_MSG（APP 在 app_evt 收此负载后 ts_msg_recv 取件）。
 * 发送方身份随槽携带（from = rt.app_id_str——审计/回信寻址）。 */
struct app_msg {
	char from[TS_PERM_MSG_NAME_MAX];
	uint8_t len;
	uint8_t data[TS_APPMGR_MSG_MAX];
};

static struct {
	struct app_msg slots[TS_APPMGR_MSG_SLOTS];
	uint8_t head, count;
} msgq[CONFIG_TS_APP_MAX_INSTANCES];

struct ts_app_rt *ts_appmgr_rt_by_str(const char *app_id_str)
{
	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		if (rt[i].used && atomic_get(&rt[i].running) != 0 &&
		    rt[i].app_id_str[0] != '\0' &&
		    strcmp(rt[i].app_id_str, app_id_str) == 0) {
			return &rt[i];
		}
	}
	return NULL;
}

ts_res_t ts_appmgr_msg_send(ts_ctx_t ctx, const char *target,
			    const uint8_t *data, uint32_t len)
{
	if (target == NULL || (len > 0 && data == NULL)) {
		return TS_E_PARAM;
	}
	if (len > TS_APPMGR_MSG_MAX) {
		return TS_E_RANGE;
	}
	/* 权限门（msg:send:<target> 名单——合同 10 留痕在 perm 层） */
	struct ts_app_rt *self = NULL;

	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		if (rt[i].used && rt[i].ctx.app_id == ctx.app_id) {
			self = &rt[i];
			break;
		}
	}
	if (self == NULL) {
		return TS_E_STATE; /* 发送方非运行实例（宿主直调无此路径） */
	}
	ts_res_t pr = ts_perm_check_msg(ctx, target);

	if (pr != TS_OK) {
		return pr;
	}
	/* 目标解析（运行中实例的串身份；多实例同 app_id = 首个命中——
	 * 命名空间按 app_id 隔离，entry 寻址不可达 DEC-04） */
	struct ts_app_rt *dst = ts_appmgr_rt_by_str(target);

	if (dst == NULL) {
		return TS_E_NOTFOUND;
	}
	size_t di = (size_t)(dst - rt);

	if (msgq[di].count >= TS_APPMGR_MSG_SLOTS) {
		return TS_E_BUSY; /* 满槽背压（不丢） */
	}
	struct app_msg *m = &msgq[di].slots[(msgq[di].head + msgq[di].count) %
					    TS_APPMGR_MSG_SLOTS];

	strncpy(m->from, self->app_id_str, sizeof(m->from) - 1);
	m->from[sizeof(m->from) - 1] = '\0';
	m->len = (uint8_t)len;
	memcpy(m->data, data, len);
	msgq[di].count++;

	/* mailbox 事件（满丢最旧 = 既有观测语义；消息本体已在槽内不丢） */
	(void)ts_appmgr_app_evt_id((uint16_t)(dst - rt + 1), TS_APP_EVT_MSG);
	return TS_OK;
}

ts_res_t ts_appmgr_msg_recv(ts_ctx_t ctx, char *from, size_t from_cap,
			    uint8_t *buf, uint32_t cap, uint32_t *len)
{
	if (len == NULL) {
		return TS_E_PARAM;
	}
	*len = 0;
	struct ts_app_rt *self = NULL;

	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		if (rt[i].used && rt[i].ctx.app_id == ctx.app_id) {
			self = &rt[i];
			break;
		}
	}
	if (self == NULL) {
		return TS_E_STATE;
	}
	size_t idx = (size_t)(self - rt);

	if (msgq[idx].count == 0) {
		return TS_OK; /* 空 = *len 0（非错误） */
	}
	struct app_msg *m = &msgq[idx].slots[msgq[idx].head];

	if (cap < m->len) {
		return TS_E_RANGE; /* 调用方缓冲不足（消息保留，不弹出） */
	}
	if (from != NULL && from_cap > 0) {
		strncpy(from, m->from, from_cap - 1);
		from[from_cap - 1] = '\0';
	}
	memcpy(buf, m->data, m->len);
	*len = m->len;
	msgq[idx].head = (msgq[idx].head + 1) % TS_APPMGR_MSG_SLOTS;
	msgq[idx].count--;
	return TS_OK;
}

uint8_t ts_appmgr_msg_pending(uint16_t app_id)
{
	struct ts_app_rt *r = rt_by_id(app_id);

	return (r == NULL) ? 0 : msgq[r - rt].count;
}

bool ts_appmgr_app_running_id(uint16_t app_id)
{
	struct ts_app_rt *r = rt_by_id(app_id);

	return r != NULL && r->used && atomic_get(&r->running) != 0;
}

bool ts_appmgr_app_running(void)
{
	return ts_appmgr_app_running_id(1); /* legacy 单 APP = entry 0 */
}

void ts_appmgr_app_stats_id(uint16_t app_id, struct ts_app_rt_stats *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	struct ts_app_rt *r = rt_by_id(app_id);

	if (r == NULL) {
		return;
	}
	out->evt_seen = r->evt_seen;
	out->tick_count = r->tick_count;
	out->health_fails = r->health_fails;
	out->mb_dropped = r->mb_dropped;
	out->init_res = r->init_res;
	out->last_evt = r->last_evt;
	out->running = ts_appmgr_app_running_id(app_id);
	out->health_failed = r->health_failed;
}

void ts_appmgr_app_stats(struct ts_app_rt_stats *out)
{
	ts_appmgr_app_stats_id(1, out); /* legacy 单 APP = entry 0 */
}

#ifdef CONFIG_TS_TEST
void ts_appmgr_app_test_reset(void)
{
	for (size_t i = 0; i < CONFIG_TS_APP_MAX_INSTANCES; i++) {
		if (rt[i].used) {
			(void)ts_appmgr_app_stop_id((uint16_t)(i + 1));
		}
		memset(&rt[i], 0, sizeof(rt[i]));
		k_msgq_purge(&mbs[i]);
		memset(&msgq[i], 0, sizeof(msgq[i])); /* P1-1：消息槽随隔离清台 */
	}
}
#endif
