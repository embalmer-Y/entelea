/* SPDX-License-Identifier: Apache-2.0 */
/* 命令-回执分发（LLD-ts-net §4）。sys 命令面 host_only（DEC-30①/DR-03）——
 * 注册者 = 框架自身；APP 的 msg/net 能力文法不可达（M2b.2 msg 类前结构性
 * 隔离），host 侧通道授权由 prov 凭证保证。
 * 信封 v1/v2 共存（DEC-40，按首键判别："op" = v1 / "ver" = v2；v1 弃用期）：
 *   v1 请求 map{"op", "args"?}                → 回执 map{"status", "data"}
 *   v2 请求 map{"ver","kind","rid","src","op","args"?}（args 内 idem?/to?）
 *        → 回执 map{"ver","kind","rid"<回带>,"status","data"}
 * v2 语义：idem 命中 = 回放缓存回执不重执行（LRU，CONFIG_TS_NET_IDEM_CACHE）；
 * to > 5000ms = 拒绝（断链窗口 6000ms〔DEC-22〕− 余量）。未知键/超集/未知
 * kind = 显式拒绝回执（fail-closed，不留静默）。命令执行路径与 APP 一致
 * （无豁免：estop-clear 经 ts_safety_clear_fault，set-time 仅数据字段）。
 * 上下文：zenoh 回调线程或测试直调（分发串行），幂等缓存无锁。 */
#include <stdio.h>
#include <string.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/net.h>
#ifdef CONFIG_TS_PERIPH
#include <ts/periph.h> /* F4：peripherals 自报（caps 块） */
#endif
#include <ts/power.h>
#include <ts/safety.h>
#include <ts/store.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "internal.h"
#include "../store/internal.h" /* ts_sha256 流式（CG5-B fs-commit 读回校验——verify.c 同型跨子系统引用） */

#define CMD_TABLE_MAX  22 /* 7 基础 sys + 3 租约（DEC-41）+ 5 部署面（MA3.1）+ prov-write（M10）+ 3 资源推送（CG5-B）+ 2 信任表（EC2）+ 余量 */
#define RESP_DATA_MAX  384 /* data 暂存上限（回执总预算 512 内扣除头） */
#define REQ_TO_MAX_MS  5000 /* 带内超时上界（DEC-40：断链窗口 6000 − 余量） */
#define RID_MAX        16
#define SRC_MAX        24
#define IDEM_KEY_MAX   16
#define IDEM_REPLY_MAX 512 /* = zenoh 回执预算（RESP_MAX），缓存回放原样字节 */
#define ARG_PAIRS_V1   3
#define ARG_PAIRS_V2   5

struct cmd_slot {
	const char *suffix; /* 如 "sys/get-info"（静态字面量，零拷贝） */
	ts_net_cmd_fn fn;
	bool gated; /* 写类（DEC-41）：仅 v2 信封 + 租约持有者 = src 可执行 */
	bool used;
};
static struct cmd_slot table[CMD_TABLE_MAX];

/* ---- sys 命令实现（§4 表）----------------------------------------------- */

static ts_res_t cmd_get_info(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	/* 身份自报（MA3.1：通配发现的回执 key 为查询通配形态，身份以载荷为准）：
	 * 自 ts_net_prefix 提取 node/cube 两段。 */
	char node[24] = "";
	char cube[24] = "";
	size_t plen = strlen(ts_net_prefix);

	if (plen > 8 && strncmp(ts_net_prefix, "tessera/", 8) == 0) {
		size_t c1 = 8, c1e = c1, c2, c2e;

		while (c1e < plen && ts_net_prefix[c1e] != '/') {
			c1e++;
		}
		c2 = c1e + 1;
		c2e = c2;
		while (c2e < plen && ts_net_prefix[c2e] != '/') {
			c2e++;
		}
		size_t nl = c1e - c1;
		size_t cl = (c2e > c2) ? (c2e - c2) : (plen - c2);

		if (nl < sizeof(node) && cl < sizeof(cube)) {
			memcpy(node, ts_net_prefix + c1, nl);
			memcpy(cube, ts_net_prefix + c2, cl);
		}
	}
	size_t p = 0;

#ifdef CONFIG_TS_APP_WAMR
	/* caps 块（CG5-A · Q-39④/HLD-contract-v2 §5.2：设备能力自报——异构
	 * 设备生态基础；Agent 部署链消费做装前匹配）。tiers 派生：large 支持
	 * ⇔ 装载缓冲 ≥ large 门（与安装期匹配矩阵同源——无独立开关）。 */
	const bool large_ok =
		CONFIG_TS_APP_LOAD_MAX >= CONFIG_TS_APP_SIZE_GATE_LARGE;

	/* F4（设备形态批）：peripherals 自报——注册表全量 uid:kind（Agent 侧
	 * 透传消费；无条件出——无外设 = 空数组，形态确定）。 */
	size_t pn = 0;

#ifdef CONFIG_TS_PERIPH
	pn = ts_periph_count();
#endif
	if (!ts_cbor_put_map(r, cap, &p, 6) ||
	    !ts_cbor_put_tstr(r, cap, &p, "fw") ||
	    !ts_cbor_put_tstr(r, cap, &p, CONFIG_TS_FW_VERSION) ||
	    !ts_cbor_put_tstr(r, cap, &p, "board") ||
	    !ts_cbor_put_tstr(r, cap, &p, CONFIG_BOARD) ||
	    !ts_cbor_put_tstr(r, cap, &p, "node") ||
	    !ts_cbor_put_tstr(r, cap, &p, node) ||
	    !ts_cbor_put_tstr(r, cap, &p, "cube") ||
	    !ts_cbor_put_tstr(r, cap, &p, cube) ||
	    !ts_cbor_put_tstr(r, cap, &p, "wall_ms") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_time_wall_ms()) ||
	    !ts_cbor_put_tstr(r, cap, &p, "caps") ||
	    !ts_cbor_put_map(r, cap, &p, 4) ||
	    !ts_cbor_put_tstr(r, cap, &p, "tiers") ||
	    !ts_cbor_put_array(r, cap, &p, large_ok ? 2U : 1U) ||
	    !ts_cbor_put_tstr(r, cap, &p, "small") ||
	    (large_ok && !ts_cbor_put_tstr(r, cap, &p, "large")) ||
	    !ts_cbor_put_tstr(r, cap, &p, "max_linear_memory_kb") ||
	    !ts_cbor_put_uint(r, cap, &p, (uint64_t)CONFIG_TS_APP_WAMR_HEAP / 1024) ||
	    !ts_cbor_put_tstr(r, cap, &p, "slot_pool_kb") ||
	    !ts_cbor_put_uint(r, cap, &p,
			      (uint64_t)CONFIG_TS_STORE_SLOT_SIZE *
				      CONFIG_TS_STORE_SLOT_COUNT / 1024) ||
	    !ts_cbor_put_tstr(r, cap, &p, "peripherals") ||
	    !ts_cbor_put_array(r, cap, &p, pn)) {
		return TS_E_IO;
	}
#ifdef CONFIG_TS_PERIPH
	static const char *pk_names[] = {"gpio", "pwm", "power", "adc"};

	for (size_t pi = 0; pi < pn; pi++) {
		const ts_periph_desc_t *pd = ts_periph_get(pi);

		if (pd == NULL || (unsigned)pd->kind > 3 ||
		    !ts_cbor_put_map(r, cap, &p, 2) ||
		    !ts_cbor_put_tstr(r, cap, &p, "uid") ||
		    !ts_cbor_put_tstr(r, cap, &p, pd->uid) ||
		    !ts_cbor_put_tstr(r, cap, &p, "kind") ||
		    !ts_cbor_put_tstr(r, cap, &p, pk_names[pd->kind])) {
			return TS_E_IO;
		}
	}
#endif
	*n = p;
	return TS_OK;
#else
	/* 无 WAMR 桩构建：无 APP 能力面——维持 v1 身份五键形态 */
	if (!ts_cbor_put_map(r, cap, &p, 5) ||
	    !ts_cbor_put_tstr(r, cap, &p, "fw") ||
	    !ts_cbor_put_tstr(r, cap, &p, CONFIG_TS_FW_VERSION) ||
	    !ts_cbor_put_tstr(r, cap, &p, "board") ||
	    !ts_cbor_put_tstr(r, cap, &p, CONFIG_BOARD) ||
	    !ts_cbor_put_tstr(r, cap, &p, "node") ||
	    !ts_cbor_put_tstr(r, cap, &p, node) ||
	    !ts_cbor_put_tstr(r, cap, &p, "cube") ||
	    !ts_cbor_put_tstr(r, cap, &p, cube) ||
	    !ts_cbor_put_tstr(r, cap, &p, "wall_ms") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_time_wall_ms())) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
#endif
}

static ts_res_t cmd_get_link(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 4) ||
	    !ts_cbor_put_tstr(r, cap, &p, "state") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_net_state()) ||
	    !ts_cbor_put_tstr(r, cap, &p, "link_up") ||
	    !ts_cbor_put_bool(r, cap, &p, ts_net_link_up()) ||
	    !ts_cbor_put_tstr(r, cap, &p, "hb_seq") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_net_linkmon_hb_seq()) ||
	    !ts_cbor_put_tstr(r, cap, &p, "pubq_dropped") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_net_pubq_dropped())) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_get_safety(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	ts_safety_summary_t s;

	if (ts_safety_summary(&s) != TS_OK) {
		return TS_E_IO;
	}
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 5) ||
	    !ts_cbor_put_tstr(r, cap, &p, "channels") ||
	    !ts_cbor_put_uint(r, cap, &p, s.channels) ||
	    !ts_cbor_put_tstr(r, cap, &p, "poweron") ||
	    !ts_cbor_put_uint(r, cap, &p, s.by_state[TS_ST_SAFE_POWERON]) ||
	    !ts_cbor_put_tstr(r, cap, &p, "active") ||
	    !ts_cbor_put_uint(r, cap, &p, s.by_state[TS_ST_ACTIVE]) ||
	    !ts_cbor_put_tstr(r, cap, &p, "linkloss") ||
	    !ts_cbor_put_uint(r, cap, &p, s.by_state[TS_ST_SAFE_LINKLOSS]) ||
	    !ts_cbor_put_tstr(r, cap, &p, "fault") ||
	    !ts_cbor_put_uint(r, cap, &p, s.by_state[TS_ST_SAFE_FAULT])) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_get_budget(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	/* ts-power 预算快照（M3b 实装；LLD-ts-power §3——prov 只读总额） */
	ARG_UNUSED(a);
	size_t p = 0;
#ifdef CONFIG_TS_POWER
	ts_power_budget_t b;

	ts_power_budget_snapshot(&b);
	if (!ts_cbor_put_map(r, cap, &p, 4) ||
	    !ts_cbor_put_tstr(r, cap, &p, "budget_ma") ||
	    !ts_cbor_put_uint(r, cap, &p, b.budget_ma) ||
	    !ts_cbor_put_tstr(r, cap, &p, "used_ma") ||
	    !ts_cbor_put_uint(r, cap, &p, b.used_ma) ||
	    !ts_cbor_put_tstr(r, cap, &p, "peak_ma") ||
	    !ts_cbor_put_uint(r, cap, &p, b.peak_ma) ||
	    !ts_cbor_put_tstr(r, cap, &p, "slots") ||
	    !ts_cbor_put_uint(r, cap, &p, b.slots)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
#else
	if (!ts_cbor_put_map(r, cap, &p, 1) ||
	    !ts_cbor_put_tstr(r, cap, &p, "note") ||
	    !ts_cbor_put_tstr(r, cap, &p, "ts-power disabled")) {
		return TS_E_IO;
	}
	*n = p;
	return TS_E_NOTFOUND;
#endif
}

#define AUDIT_EXPORT_MAX 6 /* DR-07：单次导出条数上限（回执定容内最坏编码预算） */

static ts_res_t cmd_get_audit(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	/* DEC-52①：src="flash" = 读 flash 审计环（掉电留存的安全异常，含
	 * seq/evt 字段）；缺省 = 内存环（既有语义，向后兼容）。 */
#ifdef CONFIG_TS_STORE_AUDIT
	if (a->has_src_flash) {
		static ts_store_audit_entry_t fent[AUDIT_EXPORT_MAX];
		size_t got = ts_store_audit_read(fent, AUDIT_EXPORT_MAX);
		size_t p = 0;

		if (!ts_cbor_put_map(r, cap, &p, 2) ||
		    !ts_cbor_put_tstr(r, cap, &p, "src") ||
		    !ts_cbor_put_tstr(r, cap, &p, "flash") ||
		    !ts_cbor_put_tstr(r, cap, &p, "entries") ||
		    !ts_cbor_put_array(r, cap, &p, (uint32_t)got)) {
			return TS_E_IO;
		}
		for (size_t i = 0; i < got; i++) {
			const ts_store_audit_entry_t *e = &fent[i];

			if (!ts_cbor_put_array(r, cap, &p, 7) ||
			    !ts_cbor_put_uint(r, cap, &p, e->seq) ||
			    !ts_cbor_put_uint(r, cap, &p, e->t_ms) ||
			    !ts_cbor_put_int(r, cap, &p, e->res) ||
			    !ts_cbor_put_uint(r, cap, &p, e->ch_idx) ||
			    !ts_cbor_put_uint(r, cap, &p, e->kind) ||
			    !ts_cbor_put_uint(r, cap, &p, e->actor) ||
			    !ts_cbor_put_uint(r, cap, &p, e->evt) ||
			    !ts_cbor_put_uint(r, cap, &p, e->value_u)) {
				return TS_E_IO;
			}
		}
		*n = p;
		return TS_OK;
	}
#endif
	static ts_audit_entry_t entries[AUDIT_EXPORT_MAX];
	size_t got = ts_safety_audit_copy(entries, AUDIT_EXPORT_MAX);

	/* 回执预算不足 = TS_E_IO（调用方可分片；V1 定容 512B × 16 条紧凑编码） */
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "dropped") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_safety_audit_dropped()) ||
	    !ts_cbor_put_tstr(r, cap, &p, "entries") ||
	    !ts_cbor_put_array(r, cap, &p, (uint32_t)got)) {
		return TS_E_IO;
	}
	for (size_t i = 0; i < got; i++) {
		const ts_audit_entry_t *e = &entries[i];

		/* CG3 · 合同 11：末位追加 src（写路径来源维度；加性变更——
		 * 消费端按位置取前 6 字段不受影响） */
		if (!ts_cbor_put_array(r, cap, &p, 7) ||
		    !ts_cbor_put_uint(r, cap, &p, e->t_ms) ||
		    !ts_cbor_put_int(r, cap, &p, e->res) ||
		    !ts_cbor_put_uint(r, cap, &p, e->ch_idx) ||
		    !ts_cbor_put_uint(r, cap, &p, e->kind) ||
		    !ts_cbor_put_uint(r, cap, &p, e->actor) ||
		    !ts_cbor_put_uint(r, cap, &p, e->value_u) ||
		    !ts_cbor_put_uint(r, cap, &p, e->src)) {
			return TS_E_IO;
		}
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_set_time(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	/* DR-08：仅数据字段（ts_time_wall_set）；控制路径不受影响（合同 9） */
	size_t p = 0;

	if (!a->has_time_ms) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "time_ms")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	ts_time_wall_set(a->time_ms);
	if (!ts_cbor_put_map(r, cap, &p, 1) ||
	    !ts_cbor_put_tstr(r, cap, &p, "wall_ms") ||
	    !ts_cbor_put_uint(r, cap, &p, ts_time_wall_ms())) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

/* V0.4.0 M10（Q-34/DEC-53）：维护模式 prov 写通道——lease 门控（gated 表
 * 项）+ confirm="prov" 硬令牌（estop-clear 同型）+ 写前全量 schema 校验
 * （prov_maint.c fail-closed）+ per-boot 一次性窗。生效 = 下次复位装载。 */
static ts_res_t cmd_prov_write(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;

	if (strcmp(a->confirm, "prov") != 0) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "confirm=prov")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	if (a->chunk == NULL || a->chunk_len == 0) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "data")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	ts_res_t res = ts_store_prov_write_maint(a->chunk, a->chunk_len);

	if (res != TS_OK) {
		return res; /* 校验失败/窗已用/写失败——零副作用或如实 */
	}
	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "written") ||
	    !ts_cbor_put_bool(r, cap, &p, true) ||
	    !ts_cbor_put_tstr(r, cap, &p, "applies") ||
	    !ts_cbor_put_tstr(r, cap, &p, "next-reboot")) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_estop_clear(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n){
	size_t p = 0;

	/* CG2（Q-41③ · LLD-ts-safety §5a）：prov estop_remote_clear = disabled
	 * → 远程清除通道拒（有本地交互设备的保守缺省——本地清除为主）；
	 * v1 prov 缺字段 = enabled（现网兼容）。 */
	if (ts_store_prov()->estop_remote_clear == 0) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "local-clear")) {
			*n = p;
		}
		return TS_E_STATE;
	}
	/* 确认令牌硬点（DEC-30①）：confirm != "estop" → 拒绝（防误触） */
	if (strcmp(a->confirm, "estop") != 0) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "confirm=estop")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	ts_res_t res = ts_safety_clear_fault();

	if (!ts_cbor_put_map(r, cap, &p, 1) ||
	    !ts_cbor_put_tstr(r, cap, &p, "cleared") ||
	    !ts_cbor_put_bool(r, cap, &p, res == TS_OK)) {
		return TS_E_IO;
	}
	*n = p;
	return res;
}

/* ---- sys 命令实现：控制租约（DEC-41，LLD §4.5）-------------------------- */

/* ---- 宿主资源推送（CG5-B · Q-39③ 方案 A · HLD-contract-v2 §6）------------
 * 授权 = appmgr res registry（随包 needs.resources 前缀，activate 绑定 +
 * boot 恢复）+ 租约门控（gated 表项）；完整性 = fs-begin 携带整文件
 * sha256，fs-commit 全量读回流式校验——失配/读回失败即删（fail-closed）
 * + 事件留痕（TS_EVT_RESOURCE_WRITTEN）。后端未挂载 = TS_E_IO 如实。 */
#if defined(CONFIG_TS_APP_WAMR) && defined(CONFIG_TS_HAL_FS)
#define FS_RES_MAX_BYTES 262144u /* 资源文件上限 = large 档门同值（Q-42/DEC-59） */

static struct {
	bool active;
	uint8_t app;       /* 目标实例（1..N） */
	char path[68];
	uint32_t total;
	uint32_t high_water;
	uint8_t sha[32];
} fs_stage;

static ts_res_t cmd_fs_begin(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;
	uint8_t app = a->has_app ? a->app : 1;

	if (!a->has_path || !a->has_total || !a->has_sha256) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "path+total+sha256")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	if (a->total == 0 || a->total > FS_RES_MAX_BYTES ||
	    a->path[0] != '/' || strstr(a->path, "..") != NULL) {
		return TS_E_PARAM;
	}
	if (ts_appmgr_res_allows(app, a->path) != TS_OK) {
		/* 随包白名单外 = 越权留痕（合同 10 观测面——与验签拒同型） */
		const ts_evt_t evt = {.id = TS_EVT_PERM_DENIED, .t_ms = ts_time_ms()};

		ts_evt_publish(&evt);
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "resource-prefix")) {
			*n = p;
		}
		return TS_E_PERM;
	}
	memset(&fs_stage, 0, sizeof(fs_stage));
	fs_stage.active = true;
	fs_stage.app = app;
	strncpy(fs_stage.path, a->path, sizeof(fs_stage.path) - 1);
	fs_stage.total = a->total;
	memcpy(fs_stage.sha, a->sha256, 32);
	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "staged") ||
	    !ts_cbor_put_bool(r, cap, &p, true) ||
	    !ts_cbor_put_tstr(r, cap, &p, "total") ||
	    !ts_cbor_put_uint(r, cap, &p, a->total)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_fs_chunk(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	if (!fs_stage.active) {
		return TS_E_STATE; /* 无 begin 会话 */
	}
	if (a->chunk == NULL || a->chunk_len == 0) {
		size_t p = 0;

		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "data")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	if ((uint64_t)a->offset + a->chunk_len > fs_stage.total) {
		return TS_E_PARAM; /* 越界拒绝（不静默截断） */
	}
	ts_res_t w = ts_fs_host_write(fs_stage.path, a->offset, a->chunk,
				      (uint16_t)a->chunk_len);

	if (w != TS_OK) {
		return w; /* 后端如实（未挂载 = TS_E_IO——分层可区分：授权已过 */
	}
	if (a->offset + a->chunk_len > fs_stage.high_water) {
		fs_stage.high_water = a->offset + a->chunk_len;
	}
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "written") ||
	    !ts_cbor_put_uint(r, cap, &p, a->chunk_len) ||
	    !ts_cbor_put_tstr(r, cap, &p, "high_water") ||
	    !ts_cbor_put_uint(r, cap, &p, fs_stage.high_water)) {
		return TS_E_IO;
	}
	*n = p;
	k_sleep(K_MSEC(1)); /* F6 同型同修（app-chunk 注记） */
	return TS_OK;
}

static ts_res_t cmd_fs_commit(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	if (!fs_stage.active) {
		return TS_E_STATE;
	}
	if (fs_stage.high_water < fs_stage.total) {
		size_t p = 0;

		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "data")) {
			*n = p;
		}
		return TS_E_STATE; /* 未收满不得验（fail-closed——app-verify 同型） */
	}
	/* 全量读回流式 sha256（buffer 256B——资源文件上限 256KB 无静态大缓冲） */
	static uint8_t rb[256];
	ts_sha256_ctx_t hc;
	uint32_t done = 0;
	bool ok = true;

	ts_sha256_init(&hc);
	while (done < fs_stage.total) {
		uint16_t len = (fs_stage.total - done > sizeof(rb))
				       ? (uint16_t)sizeof(rb)
				       : (uint16_t)(fs_stage.total - done);

		if (ts_fs_host_read(fs_stage.path, done, rb, &len) != TS_OK || len == 0) {
			ok = false; /* 读回失败 = 文件不完整 */
			break;
		}
		ts_sha256_update(&hc, rb, len);
		done += len;
	}
	uint8_t dig[32];

	if (ok) {
		ts_sha256_final(&hc, dig);
		ok = memcmp(dig, fs_stage.sha, 32) == 0;
	}
	if (!ok) {
		/* fail-closed：摘要失配/读回失败 → 删除残片 + 如实拒 */
		(void)ts_fs_host_delete(fs_stage.path);
		printk("[fs] host commit FAIL app=%u path=%s（摘要失配/读回失败——已删）\n",
		       (unsigned)fs_stage.app, fs_stage.path);
		memset(&fs_stage, 0, sizeof(fs_stage));
		return TS_E_IO;
	}
	const ts_evt_t evt = {.id = TS_EVT_RESOURCE_WRITTEN, .t_ms = ts_time_ms()};

	ts_evt_publish(&evt);
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "verified") ||
	    !ts_cbor_put_bool(r, cap, &p, true) ||
	    !ts_cbor_put_tstr(r, cap, &p, "bytes") ||
	    !ts_cbor_put_uint(r, cap, &p, fs_stage.total)) {
		return TS_E_IO;
	}
	*n = p;
	memset(&fs_stage, 0, sizeof(fs_stage)); /* 终态清台 */
	return TS_OK;
}
#endif /* CONFIG_TS_APP_WAMR && CONFIG_TS_HAL_FS */

/* ---- 信任根表命令面（V0.8.0 EC2 · Q-43④/DEC-60④）--------------------------
 * trust-update：args data = COSE_Sign1（payload = 表 CBOR）；gated（v2 信封
 * + 租约——密码学签名为主门，租约为会话层纵深）；编排链在 ts-appmgr
 * （verify.c：schema → ver 严格 N+1 → 现有效集签名 → 乒乓持久化 + 事件）。
 * trust-get：ver + keys 读回（未门控——公钥为公开材料，安卓哲学）。 */
#ifdef CONFIG_TS_STORE_TRUST
static ts_res_t cmd_trust_update(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;

	if (a->chunk == NULL || a->chunk_len == 0) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "data")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	ts_res_t res = ts_appmgr_trust_update(a->chunk, a->chunk_len);

	if (res != TS_OK) {
		return res;
	}
	const ts_trust_table_t *t = ts_store_trust();

	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "ver") ||
	    !ts_cbor_put_uint(r, cap, &p, t->ver) ||
	    !ts_cbor_put_tstr(r, cap, &p, "keys") ||
	    !ts_cbor_put_uint(r, cap, &p, t->n_keys)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_trust_get(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	const ts_trust_table_t *t = ts_store_trust();
	size_t p = 0;

	/* F5 定案（K3 夜间）：初版漏发 "keys" 键名（map(2) 仅一对照裸数组 =
	 * 畸形 CBOR）——严格消费方（agent cbor2）在第二对键处 EOF；真机
	 * driver 首次真实 parse 即拦下。 */
	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "ver") ||
	    !ts_cbor_put_uint(r, cap, &p, t->ver) ||
	    !ts_cbor_put_tstr(r, cap, &p, "keys") ||
	    !ts_cbor_put_array(r, cap, &p, t->n_keys)) {
		return TS_E_IO;
	}
	for (uint8_t i = 0; i < t->n_keys; i++) {
		if (!ts_cbor_put_bstr(r, cap, &p, t->keys[i], 32)) {
			return TS_E_IO;
		}
	}
	*n = p;
	return TS_OK;
}
#endif /* CONFIG_TS_STORE_TRUST */

static ts_res_t cmd_lease_acquire(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;

	if (!a->has_holder || a->holder[0] == '\0') {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "holder")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	uint64_t now = ts_time_ms();
	uint32_t id = 0;
	uint64_t exp = 0;
	ts_res_t res = ts_net_lease_acquire(a->holder, now, &id, &exp);

	if (res == TS_OK) {
		if (!ts_cbor_put_map(r, cap, &p, 2) ||
		    !ts_cbor_put_tstr(r, cap, &p, "lease_id") ||
		    !ts_cbor_put_uint(r, cap, &p, id) ||
		    !ts_cbor_put_tstr(r, cap, &p, "expires_at_ms") ||
		    !ts_cbor_put_uint(r, cap, &p, exp)) {
			return TS_E_IO;
		}
	} else if (res == TS_E_STATE && ts_safety_arb_owner() == TS_ARB_LOCAL) {
		/* CG3 · 合同 11：本地席位激活期 = 租约降级视图——回执带
		 * local_active 供调用方归因（区别于普通"他人持有"拒绝） */
		if (!ts_cbor_put_map(r, cap, &p, 1) ||
		    !ts_cbor_put_tstr(r, cap, &p, "local_active") ||
		    !ts_cbor_put_bool(r, cap, &p, true)) {
			return TS_E_IO;
		}
	} else if (res == TS_E_STATE) {
		/* 他人持有：回填当前租约供调用方归因（同信任域 host 面） */
		char cur[TS_NET_LEASE_HOLDER_MAX];
		uint32_t cid = 0;
		uint64_t cexp = 0;

		ts_net_lease_get(now, NULL, cur, sizeof(cur), &cid, &cexp);
		if (!ts_cbor_put_map(r, cap, &p, 3) ||
		    !ts_cbor_put_tstr(r, cap, &p, "holder") ||
		    !ts_cbor_put_tstr(r, cap, &p, cur) ||
		    !ts_cbor_put_tstr(r, cap, &p, "lease_id") ||
		    !ts_cbor_put_uint(r, cap, &p, cid) ||
		    !ts_cbor_put_tstr(r, cap, &p, "expires_at_ms") ||
		    !ts_cbor_put_uint(r, cap, &p, cexp)) {
			return TS_E_IO;
		}
	} else {
		return res;
	}
	*n = p;
	return res;
}

static ts_res_t cmd_lease_release(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;

	if (!a->has_holder || a->holder[0] == '\0') {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "holder")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	ts_res_t res = ts_net_lease_release(a->holder, ts_time_ms());

	if (res == TS_E_STATE) {
		char cur[TS_NET_LEASE_HOLDER_MAX];

		ts_net_lease_get(ts_time_ms(), NULL, cur, sizeof(cur), NULL, NULL);
		if (!ts_cbor_put_map(r, cap, &p, 2) ||
		    !ts_cbor_put_tstr(r, cap, &p, "released") ||
		    !ts_cbor_put_bool(r, cap, &p, false) ||
		    !ts_cbor_put_tstr(r, cap, &p, "holder") ||
		    !ts_cbor_put_tstr(r, cap, &p, cur)) {
			return TS_E_IO;
		}
	} else if (!ts_cbor_put_map(r, cap, &p, 1) ||
		   !ts_cbor_put_tstr(r, cap, &p, "released") ||
		   !ts_cbor_put_bool(r, cap, &p, res == TS_OK)) {
		return TS_E_IO;
	}
	*n = p;
	return res;
}

static ts_res_t cmd_lease_get(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	uint64_t now = ts_time_ms();
	bool valid = false;
	char holder[TS_NET_LEASE_HOLDER_MAX];
	uint32_t id = 0;
	uint64_t exp = 0;

	ts_net_lease_get(now, &valid, holder, sizeof(holder), &id, &exp);
	size_t p = 0;

	if (valid) {
		if (!ts_cbor_put_map(r, cap, &p, 4) ||
		    !ts_cbor_put_tstr(r, cap, &p, "valid") ||
		    !ts_cbor_put_bool(r, cap, &p, true) ||
		    !ts_cbor_put_tstr(r, cap, &p, "holder") ||
		    !ts_cbor_put_tstr(r, cap, &p, holder) ||
		    !ts_cbor_put_tstr(r, cap, &p, "lease_id") ||
		    !ts_cbor_put_uint(r, cap, &p, id) ||
		    !ts_cbor_put_tstr(r, cap, &p, "expires_at_ms") ||
		    !ts_cbor_put_uint(r, cap, &p, exp)) {
			return TS_E_IO;
		}
	} else if (!ts_cbor_put_map(r, cap, &p, 1) ||
		   !ts_cbor_put_tstr(r, cap, &p, "valid") ||
		   !ts_cbor_put_bool(r, cap, &p, false)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

/* ---- sys 命令实现：远程部署面（MA3.1，LLD-agent-deploy-net §3；gated = v2+租约）------ */

static ts_res_t cmd_app_begin(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;

	if (!a->has_total) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "total")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	uint8_t entry = a->has_app ? (uint8_t)(a->app - 1) : 0; /* M7b（Q-32⑤） */
	uint8_t slot = 0;
	ts_res_t res = ts_appmgr_stage_begin_n(a->total, entry, &slot);

	if (res != TS_OK) {
		return res; /* 尺寸非法：PARAM（data 空，status 已达） */
	}
	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "slot") ||
	    !ts_cbor_put_uint(r, cap, &p, slot) ||
	    !ts_cbor_put_tstr(r, cap, &p, "total") ||
	    !ts_cbor_put_uint(r, cap, &p, a->total)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_app_chunk(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	size_t p = 0;

	if (!a->has_offset || a->chunk == NULL || a->chunk_len == 0) {
		if (ts_cbor_put_map(r, cap, &p, 1) && ts_cbor_put_tstr(r, cap, &p, "need") &&
		    ts_cbor_put_tstr(r, cap, &p, "offset+data")) {
			*n = p;
		}
		return TS_E_PARAM;
	}
	uint32_t hw = 0;
	ts_res_t res = ts_appmgr_stage_chunk(a->offset, a->chunk, a->chunk_len, &hw);

	if (res != TS_OK) {
		return res;
	}
	if (!ts_cbor_put_map(r, cap, &p, 2) ||
	    !ts_cbor_put_tstr(r, cap, &p, "written") ||
	    !ts_cbor_put_uint(r, cap, &p, a->chunk_len) ||
	    !ts_cbor_put_tstr(r, cap, &p, "high_water") ||
	    !ts_cbor_put_uint(r, cap, &p, hw)) {
		return TS_E_IO;
	}
	*n = p;
	/* F6 修复（真机 200 分块流定案）：zenoh 读任务继承创建者优先级
	 * （pthread INHERIT_SCHED → main prio 0），背靠背分块流期间无阻塞
	 * 连转 → 低优先级线程（APP prio 10 / 喂狗路径）整个流期 queued →
	 * APPMGR WDT 复位（noinit 0x20001——非死锁，饥饿）。每分块 1ms
	 * 让步 = 412KB 包全流 +200ms（吞吐无关紧要），低优先级线程得以
	 * 调度喂狗。资源分块（fs-chunk）同型同修。 */
	k_sleep(K_MSEC(1));
	return TS_OK;
}

static ts_res_t cmd_app_verify(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	/* EC2（Q-43⑤）：验签根 = 有效信任集（工厂根 pk0/pk1 ∪ 信任表 keys
	 * ——verify_slot NULL 路径；此前单取 pk0）。 */
	uint32_t ml = 0, wl = 0, co = 0;
	ts_res_t res = ts_appmgr_stage_verify(NULL, &ml, &wl, &co);

	if (res != TS_OK) {
		return res;
	}
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 3) ||
	    !ts_cbor_put_tstr(r, cap, &p, "manifest_len") ||
	    !ts_cbor_put_uint(r, cap, &p, ml) ||
	    !ts_cbor_put_tstr(r, cap, &p, "wasm_len") ||
	    !ts_cbor_put_uint(r, cap, &p, wl) ||
	    !ts_cbor_put_tstr(r, cap, &p, "cose_off") ||
	    !ts_cbor_put_uint(r, cap, &p, co)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_app_activate(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	uint8_t entry = a->has_app ? (uint8_t)(a->app - 1) : 0; /* M7b（Q-32⑤） */
	ts_app_info_t info;
	ts_res_t res = ts_appmgr_stage_activate_n(entry, &info);

	/* F3 · DEC-62①：安装闸 = 结构化中间态回执（非失败——status=-13 +
	 * data {pending, slot}）。确认仅固件本地路；Agent 链按「待本地确认」
	 * 收尾（呈现面 = EC3 无头呈报同族）。 */
	if (res == TS_E_PENDING) {
		size_t p = 0;

		if (!ts_cbor_put_map(r, cap, &p, 2) ||
		    !ts_cbor_put_tstr(r, cap, &p, "pending") ||
		    !ts_cbor_put_uint(r, cap, &p, 1) ||
		    !ts_cbor_put_tstr(r, cap, &p, "slot") ||
		    !ts_cbor_put_uint(r, cap, &p, info.pending_slot)) {
			return TS_E_STATE; /* 回执编码失败 = 拒（不裸露半编码面） */
		}
		*n = p;
		return TS_E_PENDING;
	}
	if (res != TS_OK) {
		return res;
	}
	size_t p = 0;

	if (!ts_cbor_put_map(r, cap, &p, 3) ||
	    !ts_cbor_put_tstr(r, cap, &p, "state") ||
	    !ts_cbor_put_uint(r, cap, &p, (uint64_t)info.state) ||
	    !ts_cbor_put_tstr(r, cap, &p, "active_slot") ||
	    !ts_cbor_put_uint(r, cap, &p, info.active_slot) ||
	    !ts_cbor_put_tstr(r, cap, &p, "rollback_count") ||
	    !ts_cbor_put_uint(r, cap, &p, info.rollback_count)) {
		return TS_E_IO;
	}
	*n = p;
	return TS_OK;
}

static ts_res_t cmd_get_app(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	uint8_t entry = a->has_app ? (uint8_t)(a->app - 1) : 0; /* M7b（Q-32⑤） */
	ts_app_info_t info;
	ts_res_t res = ts_appmgr_get_info_n(entry, &info);

	if (res != TS_OK) {
		return res;
	}
	size_t p = 0;

	/* 顶层字段 = 请求实例（缺省 1，旧客户端等价）视图；
	 * apps 数组 = 全部实例位全景（M7b；未部署位如实 STAGED/空 id）。 */
	if (!ts_cbor_put_map(r, cap, &p, 5) ||
	    !ts_cbor_put_tstr(r, cap, &p, "state") ||
	    !ts_cbor_put_uint(r, cap, &p, (uint64_t)info.state) ||
	    !ts_cbor_put_tstr(r, cap, &p, "active_slot") ||
	    !ts_cbor_put_uint(r, cap, &p, info.active_slot) ||
	    !ts_cbor_put_tstr(r, cap, &p, "rollback_count") ||
	    !ts_cbor_put_uint(r, cap, &p, info.rollback_count) ||
	    !ts_cbor_put_tstr(r, cap, &p, "app_id") ||
	    !ts_cbor_put_tstr(r, cap, &p, info.app_id[0] != '\0' ? info.app_id : "") ||
	    !ts_cbor_put_tstr(r, cap, &p, "apps") ||
	    !ts_cbor_put_array(r, cap, &p, CONFIG_TS_APP_MAX_INSTANCES)) {
		return TS_E_IO;
	}
	for (uint8_t e = 0; e < CONFIG_TS_APP_MAX_INSTANCES; e++) {
		ts_app_info_t ei;

		if (ts_appmgr_get_info_n(e, &ei) != TS_OK ||
		    !ts_cbor_put_map(r, cap, &p, 5) ||
		    !ts_cbor_put_tstr(r, cap, &p, "app") ||
		    !ts_cbor_put_uint(r, cap, &p, (uint64_t)(e + 1)) ||
		    !ts_cbor_put_tstr(r, cap, &p, "state") ||
		    !ts_cbor_put_uint(r, cap, &p, (uint64_t)ei.state) ||
		    !ts_cbor_put_tstr(r, cap, &p, "active_slot") ||
		    !ts_cbor_put_uint(r, cap, &p, ei.active_slot) ||
		    !ts_cbor_put_tstr(r, cap, &p, "rollback_count") ||
		    !ts_cbor_put_uint(r, cap, &p, ei.rollback_count) ||
		    !ts_cbor_put_tstr(r, cap, &p, "app_id") ||
		    !ts_cbor_put_tstr(r, cap, &p,
				      ei.app_id[0] != '\0' ? ei.app_id : "")) {
			return TS_E_IO;
		}
	}
	*n = p;
	return TS_OK;
}

/* ---- 注册与分发 ---------------------------------------------------------- */

ts_res_t ts_net_cmd_register(const char *suffix, ts_net_cmd_fn fn)
{
	if (suffix == NULL || suffix[0] == '\0' || fn == NULL) {
		return TS_E_PARAM;
	}
	for (int i = 0; i < CMD_TABLE_MAX; i++) {
		if (table[i].used && strcmp(table[i].suffix, suffix) == 0) {
			return TS_E_PARAM; /* 撞名 */
		}
	}
	for (int i = 0; i < CMD_TABLE_MAX; i++) {
		if (!table[i].used) {
			table[i].suffix = suffix;
			table[i].fn = fn;
			table[i].gated = false; /* 公共注册面 = 未门控（gated 属 sys 内部装配） */
			table[i].used = true;
			return TS_OK;
		}
	}
	return TS_E_NOMEM;
}

static struct cmd_slot *find_cmd(const char *suffix)
{
	for (int i = 0; i < CMD_TABLE_MAX; i++) {
		if (table[i].used && strcmp(table[i].suffix, suffix) == 0) {
			return &table[i];
		}
	}
	return NULL;
}

/* ---- 请求解析（v1/v2 统一体；fail-closed：未知键/超集/重复键 = 拒绝）----- */

struct req_env {
	bool v2;
	char rid[RID_MAX + 1];
	char src[SRC_MAX + 1]; /* 进审计 payload 的调用方身份（DEC-40） */
	char op[24];
	ts_net_cmd_args_t args;
	bool has_idem;
	char idem[IDEM_KEY_MAX + 1];
	bool has_to;
	uint32_t to_ms;
};

/* args 体公共键（confirm/time_ms/holder）；idem/to 仅 v2 信封（DEC-40） */
static bool parse_args_body(ts_cbor_rd_t *r, uint32_t pairs, struct req_env *e)
{
	for (uint32_t j = 0; j < pairs; j++) {
		char akey[12];

		if (!ts_cbor_tstr(r, akey, sizeof(akey))) {
			return false;
		}
		if (strcmp(akey, "confirm") == 0) {
			if (!ts_cbor_tstr(r, e->args.confirm, sizeof(e->args.confirm))) {
				return false;
			}
		} else if (strcmp(akey, "time_ms") == 0) {
			uint64_t v;

			if (!ts_cbor_uint(r, &v)) {
				return false;
			}
			e->args.time_ms = v;
			e->args.has_time_ms = true;
		} else if (strcmp(akey, "holder") == 0) {
			if (!ts_cbor_tstr(r, e->args.holder, sizeof(e->args.holder))) {
				return false;
			}
			e->args.has_holder = true;
		} else if (e->v2 && strcmp(akey, "total") == 0) {
			uint64_t v;

			if (!ts_cbor_uint(r, &v) || v > UINT32_MAX) {
				return false;
			}
			e->args.total = (uint32_t)v;
			e->args.has_total = true;
		} else if (e->v2 && strcmp(akey, "app") == 0) {
			/* M7b（Q-32⑤）：目标实例（app_id 1..N；越界 = 请求非法） */
			uint64_t v;

			if (!ts_cbor_uint(r, &v) || v == 0 ||
			    v > CONFIG_TS_APP_MAX_INSTANCES) {
				return false;
			}
			e->args.app = (uint8_t)v;
			e->args.has_app = true;
		} else if (e->v2 && strcmp(akey, "offset") == 0) {
			uint64_t v;

			if (!ts_cbor_uint(r, &v) || v > UINT32_MAX) {
				return false;
			}
			e->args.offset = (uint32_t)v;
			e->args.has_offset = true;
		} else if (e->v2 && strcmp(akey, "audit_src") == 0) {
			/* DEC-52①：get-audit audit_src="flash"（读 flash 环）；其他值
			 * 忽略（缺省 = 内存环，向后兼容）。键名避开信封层 src 身份。 */
			char src[8];

			if (!ts_cbor_tstr(r, src, sizeof(src))) {
				return false;
			}
			if (strcmp(src, "flash") == 0) {
				e->args.has_src_flash = true;
			}
		} else if (e->v2 && strcmp(akey, "data") == 0) {
			/* bstr 零拷贝引用（生存期 = 分发调用域）；块长上限 =
			 * CONFIG_TS_NET_APP_CHUNK_MAX（LLD-agent-deploy-net §3：默认 2048/上限 4096） */
			if (!ts_cbor_bstr_ref(r, &e->args.chunk, &e->args.chunk_len) ||
			    e->args.chunk_len > CONFIG_TS_NET_APP_CHUNK_MAX) {
				return false;
			}
		} else if (e->v2 && strcmp(akey, "path") == 0) {
			/* CG5-B（方案 A）：fs-begin 资源目标路径（绝对路径、禁 ..——
			 * 文法校验在 handler；此处仅解码） */
			if (!ts_cbor_tstr(r, e->args.path, sizeof(e->args.path))) {
				return false;
			}
			e->args.has_path = true;
		} else if (e->v2 && strcmp(akey, "sha256") == 0) {
			/* CG5-B：整文件摘要（bstr 恰 32B——fs-commit 读回校验对象） */
			const uint8_t *d;
			uint32_t dl;

			if (!ts_cbor_bstr_ref(r, &d, &dl) || dl != 32) {
				return false;
			}
			memcpy(e->args.sha256, d, 32);
			e->args.has_sha256 = true;
		} else if (e->v2 && strcmp(akey, "idem") == 0) {
			if (!ts_cbor_tstr(r, e->idem, sizeof(e->idem))) {
				return false;
			}
			e->has_idem = true;
		} else if (e->v2 && strcmp(akey, "to") == 0) {
			uint64_t v;

			if (!ts_cbor_uint(r, &v) || v > UINT32_MAX) {
				return false;
			}
			e->to_ms = (uint32_t)v;
			e->has_to = true;
		} else {
			return false; /* 未知参数/v1 带 v2 键 = 拒绝 */
		}
	}
	return true;
}

/* 首键判别（DEC-40）："op" = v1 map{op, args?}；"ver" = v2 信封。
 * v2 必含 ver/kind/rid/src/op（args 可选）；kind 仅接受 1（sys 命令请求，
 * LLD §4.4 注册表）；ver 仅接受 1（未来版本 = fail-closed）。 */
static ts_res_t parse_req(const uint8_t *req, uint32_t req_len, struct req_env *e)
{
	ts_cbor_rd_t r;
	uint32_t pairs;

	ts_cbor_rd_init(&r, req, req_len);
	memset(e, 0, sizeof(*e));
	if (!ts_cbor_map_open(&r, &pairs) || pairs == 0) {
		return TS_E_PARAM;
	}
	char first[8];

	if (!ts_cbor_tstr(&r, first, sizeof(first))) {
		return TS_E_PARAM;
	}
	if (strcmp(first, "op") == 0) {
		e->v2 = false;
		if (pairs > 2) {
			return TS_E_PARAM;
		}
		if (!ts_cbor_tstr(&r, e->op, sizeof(e->op))) {
			return TS_E_PARAM;
		}
		if (pairs == 2) {
			char key[8];

			if (!ts_cbor_tstr(&r, key, sizeof(key)) || strcmp(key, "args") != 0 ||
			    !ts_cbor_map_open(&r, &pairs) || pairs > ARG_PAIRS_V1 ||
			    !parse_args_body(&r, pairs, e)) {
				return TS_E_PARAM;
			}
		}
		return TS_OK;
	}
	if (strcmp(first, "ver") != 0) {
		return TS_E_PARAM; /* 未知首键 = 非法形态 */
	}
	e->v2 = true;
	if (pairs > 6) {
		return TS_E_PARAM;
	}
	uint64_t ver = 0;

	if (!ts_cbor_uint(&r, &ver) || ver != 1) {
		return TS_E_PARAM;
	}
	bool have_kind = false, have_rid = false, have_src = false, have_op = false;
	bool have_args = false;

	for (uint32_t i = 1; i < pairs; i++) {
		char key[8];

		if (!ts_cbor_tstr(&r, key, sizeof(key))) {
			return TS_E_PARAM;
		}
		if (strcmp(key, "kind") == 0) {
			uint64_t v;

			if (have_kind || !ts_cbor_uint(&r, &v)) {
				return TS_E_PARAM;
			}
			have_kind = true;
			if (v != 1) {
				return TS_E_PARAM;
			}
		} else if (strcmp(key, "rid") == 0) {
			if (have_rid || !ts_cbor_tstr(&r, e->rid, sizeof(e->rid))) {
				return TS_E_PARAM;
			}
			have_rid = true;
		} else if (strcmp(key, "src") == 0) {
			if (have_src || !ts_cbor_tstr(&r, e->src, sizeof(e->src))) {
				return TS_E_PARAM;
			}
			have_src = true;
		} else if (strcmp(key, "op") == 0) {
			if (have_op || !ts_cbor_tstr(&r, e->op, sizeof(e->op))) {
				return TS_E_PARAM;
			}
			have_op = true;
		} else if (strcmp(key, "args") == 0) {
			if (have_args || !ts_cbor_map_open(&r, &pairs) ||
			    pairs > ARG_PAIRS_V2 || !parse_args_body(&r, pairs, e)) {
				return TS_E_PARAM;
			}
			have_args = true;
		} else {
			return TS_E_PARAM; /* 未知信封键 = 拒绝 */
		}
	}
	if (!have_kind || !have_rid || !have_src || !have_op) {
		return TS_E_PARAM;
	}
	return TS_OK;
}

/* ---- 幂等回执缓存（DEC-40：同 idem 重发 = 回放不重执行）------------------ */

struct idem_slot {
	bool used;
	uint64_t stamp; /* LRU 时钟（单调，非墙钟——合同 9） */
	char idem[IDEM_KEY_MAX + 1];
	char op[24];
	ts_res_t status;
	size_t rlen;
	uint8_t reply[IDEM_REPLY_MAX];
};
static struct idem_slot idem_cache[CONFIG_TS_NET_IDEM_CACHE];
static uint64_t idem_clock;

static struct idem_slot *idem_find(const char *idem_key)
{
	for (int i = 0; i < CONFIG_TS_NET_IDEM_CACHE; i++) {
		if (idem_cache[i].used && strcmp(idem_cache[i].idem, idem_key) == 0) {
			return &idem_cache[i];
		}
	}
	return NULL;
}

static void idem_insert(const struct req_env *e, const uint8_t *reply, size_t rlen,
			ts_res_t status)
{
	if (rlen > IDEM_REPLY_MAX) {
		return; /* 防御：回执预算即 512，不可达分支 */
	}
	struct idem_slot *s = NULL;

	for (int i = 0; i < CONFIG_TS_NET_IDEM_CACHE; i++) {
		if (!idem_cache[i].used) {
			s = &idem_cache[i];
			break;
		}
	}
	if (s == NULL) { /* 满则驱逐最旧（定容 LRU） */
		s = &idem_cache[0];
		for (int i = 1; i < CONFIG_TS_NET_IDEM_CACHE; i++) {
			if (idem_cache[i].stamp < s->stamp) {
				s = &idem_cache[i];
			}
		}
	}
	s->used = true;
	strcpy(s->idem, e->idem);
	strcpy(s->op, e->op);
	s->status = status;
	memcpy(s->reply, reply, rlen);
	s->rlen = rlen;
	s->stamp = ++idem_clock;
}

/* ---- 回执编码（v1 两对 / v2 五对信封，rid 回带）-------------------------- */

/* 兜底：data = map{"error": tstr}（status 已定）；v2 加信封头（rid 可能空） */
static ts_res_t encode_err(uint8_t *resp, size_t cap, size_t *resp_len, const struct req_env *e,
			   ts_res_t status, const char *note)
{
	size_t p = 0;
	bool ok;

	if (e->v2) {
		ok = ts_cbor_put_map(resp, cap, &p, 5) &&
		     ts_cbor_put_tstr(resp, cap, &p, "ver") &&
		     ts_cbor_put_uint(resp, cap, &p, 1) &&
		     ts_cbor_put_tstr(resp, cap, &p, "kind") &&
		     ts_cbor_put_uint(resp, cap, &p, 16) &&
		     ts_cbor_put_tstr(resp, cap, &p, "rid") &&
		     ts_cbor_put_tstr(resp, cap, &p, e->rid) &&
		     ts_cbor_put_tstr(resp, cap, &p, "status") &&
		     ts_cbor_put_int(resp, cap, &p, status);
	} else {
		ok = ts_cbor_put_map(resp, cap, &p, 2) &&
		     ts_cbor_put_tstr(resp, cap, &p, "status") &&
		     ts_cbor_put_int(resp, cap, &p, status);
	}
	if (!ok || !ts_cbor_put_tstr(resp, cap, &p, "data") ||
	    !ts_cbor_put_map(resp, cap, &p, 1) ||
	    !ts_cbor_put_tstr(resp, cap, &p, "error") ||
	    !ts_cbor_put_tstr(resp, cap, &p, note)) {
		return TS_E_IO;
	}
	*resp_len = p;
	return status;
}

/* 成功路径：handler 先写 data 暂存，再回填头（status 事后才知） */
static ts_res_t wrap_reply(uint8_t *resp, size_t cap, size_t *resp_len, const struct req_env *e,
			   ts_res_t status, const uint8_t *data, size_t data_len)
{
	size_t p = 0;
	bool ok;

	if (e->v2) {
		ok = ts_cbor_put_map(resp, cap, &p, 5) &&
		     ts_cbor_put_tstr(resp, cap, &p, "ver") &&
		     ts_cbor_put_uint(resp, cap, &p, 1) &&
		     ts_cbor_put_tstr(resp, cap, &p, "kind") &&
		     ts_cbor_put_uint(resp, cap, &p, 16) &&
		     ts_cbor_put_tstr(resp, cap, &p, "rid") &&
		     ts_cbor_put_tstr(resp, cap, &p, e->rid) &&
		     ts_cbor_put_tstr(resp, cap, &p, "status") &&
		     ts_cbor_put_int(resp, cap, &p, status);
	} else {
		ok = ts_cbor_put_map(resp, cap, &p, 2) &&
		     ts_cbor_put_tstr(resp, cap, &p, "status") &&
		     ts_cbor_put_int(resp, cap, &p, status);
	}
	if (!ok || !ts_cbor_put_tstr(resp, cap, &p, "data") ||
	    data_len > cap - p) {
		return TS_E_IO;
	}
	if (data_len == 0) {
		/* F5 同族（K3 真机定案）：data 空值必须编码为空 map（a0）——
		 * handler 内部失败（如验签拒）不带 data 时，键后零字节 =
		 * 畸形 CBOR，严格消费方（agent cbor2）在键值处 EOF。 */
		if (!ts_cbor_put_map(resp, cap, &p, 0)) {
			return TS_E_IO;
		}
	} else {
		memcpy(resp + p, data, data_len);
		p += data_len;
	}
	*resp_len = p;
	return status;
}

ts_res_t ts_net_cmd_dispatch(const char *key_suffix, const uint8_t *req, uint32_t req_len,
			     uint8_t *resp, size_t cap, size_t *resp_len)
{
	if (key_suffix == NULL || resp == NULL || resp_len == NULL) {
		return TS_E_PARAM;
	}
	*resp_len = 0;
	struct cmd_slot *slot = find_cmd(key_suffix);

	if (slot == NULL) {
		struct req_env e = {0};

		return encode_err(resp, cap, resp_len, &e, TS_E_NOTFOUND, "unknown key");
	}
	if (req == NULL || req_len == 0) {
		struct req_env e = {0};

		return encode_err(resp, cap, resp_len, &e, TS_E_PARAM, "empty request");
	}
	struct req_env e;

	if (parse_req(req, req_len, &e) != TS_OK) {
		return encode_err(resp, cap, resp_len, &e, TS_E_PARAM, "malformed request");
	}
	/* to 上界（DEC-40）：命令执行上界须留断链判定余量 */
	if (e.v2 && e.has_to && e.to_ms > REQ_TO_MAX_MS) {
		return encode_err(resp, cap, resp_len, &e, TS_E_PARAM, "to>5000ms rejected");
	}
	/* key 后缀与 op 双重匹配（sys/get-info 处 op 必须为 get-info——防表项错位）。
	 * F-4（docs/history/review-archive）：匹配先于 idem 回放——回执只应在与其执行同 key 的
	 * 查询上回放，跨 key 同 idem = op/key mismatch 拒绝（原实现回放先于
	 * 匹配，语义松：同 idem 可在任意 key 得到回执）。 */
	char expect[36];

	int w = snprintf(expect, sizeof(expect), "sys/%s", e.op);

	if (w < 0 || (size_t)w >= sizeof(expect) ||
	    strncmp(key_suffix, "sys/", 4) != 0 ||
	    strcmp(key_suffix, expect) != 0) {
		return encode_err(resp, cap, resp_len, &e, TS_E_NOTFOUND, "op/key mismatch");
	}
	/* 幂等回放（DEC-40）：同 idem 命中 = 原样回执不重执行（门控豁免是有意
	 * 语义：回放无新副作用，缓存回执对应一次已通过门控的历史执行） */
	if (e.v2 && e.has_idem) {
		struct idem_slot *hit = idem_find(e.idem);

		if (hit != NULL) {
			if (strcmp(hit->op, e.op) != 0) {
				return encode_err(resp, cap, resp_len, &e, TS_E_PARAM,
						  "idem/op mismatch");
			}
			if (hit->rlen > cap) {
				return encode_err(resp, cap, resp_len, &e, TS_E_IO,
						  "resp overflow");
			}
			memcpy(resp, hit->reply, hit->rlen);
			*resp_len = hit->rlen;
			hit->stamp = ++idem_clock; /* LRU 触碰 */
			return hit->status;
		}
	}
	/* 写类命令门控（DEC-41 准入挂钩首个落点——部署面）：仅 v2 信封（v1 无
	 * src 身份）且当前租约持有者 = src。只读族与 estop-clear 豁免不变。 */
	if (slot->gated) {
		if (!e.v2) {
			return encode_err(resp, cap, resp_len, &e, TS_E_PARAM,
					  "v2 envelope required");
		}
		if (!ts_net_lease_held_by(e.src, ts_time_ms())) {
			return encode_err(resp, cap, resp_len, &e, TS_E_STATE,
					  "lease required");
		}
	}
	static uint8_t data_scratch[RESP_DATA_MAX]; /* 分发上下文串行（传输回调/测试） */
	size_t data_len = 0;
	ts_res_t status = slot->fn(&e.args, data_scratch, sizeof(data_scratch), &data_len);

	if (status == TS_E_IO || data_len + 64 > cap) {
		return encode_err(resp, cap, resp_len, &e, TS_E_IO, "resp overflow");
	}
	ts_res_t wr = wrap_reply(resp, cap, resp_len, &e, status, data_scratch, data_len);

	if (wr == TS_E_IO) {
		return encode_err(resp, cap, resp_len, &e, TS_E_IO, "resp overflow");
	}
	if (e.v2 && e.has_idem) {
		idem_insert(&e, resp, *resp_len, status);
	}
	return wr;
}

ts_res_t ts_net_cmd_sys_init(void)
{
	/* sys 命令表（LLD §4；host_only——本函数仅框架 init 调用）。
	 * gated = 写类（v2 信封 + 租约持有者 = src 才执行，DEC-41）。 */
	static const struct {
		const char *suffix;
		ts_net_cmd_fn fn;
		bool gated;
	} sys_cmds[] = {
		{"sys/get-info", cmd_get_info, false},
		{"sys/get-link", cmd_get_link, false},
		{"sys/get-safety", cmd_get_safety, false},
		{"sys/get-budget", cmd_get_budget, false},
		{"sys/get-audit", cmd_get_audit, false},
		{"sys/set-time", cmd_set_time, false},
		{"sys/estop-clear", cmd_estop_clear, false},
		{"sys/prov-write", cmd_prov_write, true}, /* M10（Q-34）：gated = v2+租约 */
		{"sys/lease-acquire", cmd_lease_acquire, false},
		{"sys/lease-release", cmd_lease_release, false},
		{"sys/lease-get", cmd_lease_get, false},
		{"sys/app-begin", cmd_app_begin, true},
		{"sys/app-chunk", cmd_app_chunk, true},
		{"sys/app-verify", cmd_app_verify, true},
		{"sys/app-activate", cmd_app_activate, true},
#if defined(CONFIG_TS_APP_WAMR) && defined(CONFIG_TS_HAL_FS)
		{"sys/fs-begin", cmd_fs_begin, true},   /* CG5-B：资源推送（gated+租约） */
		{"sys/fs-chunk", cmd_fs_chunk, true},
		{"sys/fs-commit", cmd_fs_commit, true},
#endif
#ifdef CONFIG_TS_STORE_TRUST
		{"sys/trust-update", cmd_trust_update, true}, /* EC2（Q-43④）：gated = v2+租约（签名为主门） */
		{"sys/trust-get", cmd_trust_get, false},      /* 公钥读回（未门控） */
#endif
		{"sys/get-app", cmd_get_app, false},
	};
	for (size_t i = 0; i < sizeof(sys_cmds) / sizeof(sys_cmds[0]); i++) {
		ts_res_t r = ts_net_cmd_register(sys_cmds[i].suffix, sys_cmds[i].fn);

		if (r != TS_OK) {
			return r; /* 表满/撞名 = 装配错误，init 如实上抛（boot fail-safe） */
		}
		/* F-3（docs/history/review-archive）：gated 位按 suffix 回查回填——公共注册面
		 * 占首个空位，若本表不是表的首个注册方，数组下标与表位错开；
		 * 按名回填使门控位恒指向本命令（按下标回填错位 = 写命令未门控，
		 * 不可接受）。注册已成功 → find 必中（assert 防御装配回归）。 */
		struct cmd_slot *s = find_cmd(sys_cmds[i].suffix);

		__ASSERT(s != NULL, "sys cmd registered but unfound");
		if (s != NULL) {
			s->gated = sys_cmds[i].gated;
		}
	}
	return TS_OK;
}

#ifdef CONFIG_TS_TEST
void ts_net_cmd_test_reset(void)
{
	memset(table, 0, sizeof(table));
	memset(idem_cache, 0, sizeof(idem_cache));
	idem_clock = 0;
}
#endif
