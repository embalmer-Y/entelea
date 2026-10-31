/* SPDX-License-Identifier: Apache-2.0 */
/* ts-interact 本地交互终端（LLD-ts-interact §1/§2；R5 · DEC-65）。
 * 薄命令层：核心逻辑函数（ts_interact_*）与 shell 运行时解耦——
 * 返回 rc + 出参，twister 直测（framework.interact）；shell 命令表仅做
 * 打印。挂点零新安全面：confirm/reject 复用 appmgr 挂起态门（60s
 * fail-closed 不变）、arb/清除复用 safety 本地路——与 BOOT 手势语义逐一
 * 对应，终端 = 物理在场代理（DEC-65②）。 */
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <ts/appmgr.h>
#include <ts/safety.h>
#include <ts/store.h>
LOG_MODULE_REGISTER(ts_interact, CONFIG_TS_INTERACT_LOG_LEVEL);

/* ---- 核心逻辑（shell 无关，测试面；声明见 ts_interact.h）----------------- */

ts_res_t ts_interact_confirm(ts_app_info_t *out)
{
	if (!ts_appmgr_pending_active()) {
		return TS_E_STATE; /* 无挂起（= 非 pending 期手势模态外） */
	}
	return ts_appmgr_confirm_pending(out);
}

ts_res_t ts_interact_reject(void)
{
	if (!ts_appmgr_pending_active()) {
		return TS_E_STATE;
	}
	return ts_appmgr_reject_pending();
}

ts_res_t ts_interact_arb(bool local)
{
	/* CG3：set_arb 幂等 + 切换零输出（EN 60204-1 §9.2.3 同型）——终端与
	 * BOOT 双短按同入口 */
	return ts_safety_set_arb(local ? TS_ARB_LOCAL : TS_ARB_REMOTE);
}

ts_res_t ts_interact_estop_clear(void)
{
	/* F2 本地路：无 fault 时 clear 如实拒（E_STATE），不发明新语义 */
	return ts_safety_clear_fault();
}

ts_res_t ts_interact_log_level(const char *lvl)
{
	if (lvl == NULL) {
		return TS_E_PARAM;
	}
	uint32_t level;

	if (strcmp(lvl, "off") == 0) {
		level = LOG_LEVEL_NONE;
	} else if (strcmp(lvl, "err") == 0) {
		level = LOG_LEVEL_ERR;
	} else if (strcmp(lvl, "wrn") == 0) {
		level = LOG_LEVEL_WRN;
	} else if (strcmp(lvl, "inf") == 0) {
		level = LOG_LEVEL_INF;
	} else if (strcmp(lvl, "dbg") == 0) {
		level = LOG_LEVEL_DBG;
	} else {
		return TS_E_PARAM; /* LLD §2：域外 fail-closed */
	}
#if defined(CONFIG_LOG)
	/* 全局详度（domain 0 全源；backend NULL = 前后端统一） */
	uint32_t n = log_src_cnt_get(0);

	for (uint32_t i = 0; i < n; i++) {
		log_filter_set(NULL, 0, (int16_t)i, level);
	}
	return TS_OK;
#else
	ARG_UNUSED(level);
	return TS_E_NOT_CONFIGURED; /* 无 log 构建 = 面不在（如实） */
#endif
}

/* ---- shell 命令层（打印薄壳；SHELL 在场才编译——core 用例 SHELL=n 可跑）-- */
#if defined(CONFIG_SHELL)

static void print_confirm(const struct shell *sh, ts_res_t rc,
			  const ts_app_info_t *out)
{
	if (rc == TS_OK) {
		shell_print(sh, "[tsi] confirm rc=0 app=%s slot=%u（暖复位装载）",
			    out->app_id, (unsigned)out->active_slot);
	} else {
		shell_print(sh, "[tsi] confirm rc=%d", (int)rc);
	}
}

static int cmd_ts_pending(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!ts_appmgr_pending_active()) {
		shell_print(sh, "[tsi] pending none");
		return 0;
	}
	char sum[160];
	int n = ts_appmgr_pending_summary(sum, sizeof(sum));

	shell_print(sh, "[tsi] pending active%s%s", n > 0 ? " " : "",
		    n > 0 ? sum : "");
	return 0;
}

static int cmd_ts_confirm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ts_app_info_t out;
	ts_res_t rc = ts_interact_confirm(&out);

	print_confirm(sh, rc, &out);
	return 0;
}

static int cmd_ts_reject(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ts_res_t rc = ts_interact_reject();

	shell_print(sh, "[tsi] reject rc=%d", (int)rc);
	return 0;
}

static int cmd_ts_arb(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 ||
	    (strcmp(argv[1], "local") != 0 && strcmp(argv[1], "remote") != 0)) {
		shell_print(sh, "[tsi] arb usage: ts arb <local|remote>");
		return -EINVAL;
	}
	ts_res_t rc = ts_interact_arb(strcmp(argv[1], "local") == 0);

	shell_print(sh, "[tsi] arb %s rc=%d", argv[1], (int)rc);
	return 0;
}

static int cmd_ts_estop(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "clear") != 0) {
		shell_print(sh, "[tsi] estop usage: ts estop clear");
		return -EINVAL;
	}
	ts_res_t rc = ts_interact_estop_clear();

	shell_print(sh, "[tsi] estop clear rc=%d", (int)rc);
	return 0;
}

static int cmd_ts_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	const ts_prov_t *prov = ts_store_prov();
	ts_arb_owner_t arb = ts_safety_arb_owner();

	shell_print(sh, "[tsi] info mode=%s arb=%s pending=%s",
		    prov->deploy_mode == TS_DEPLOY_STANDALONE ? "standalone"
							      : "orchestrated",
		    arb == TS_ARB_LOCAL ? "local" : "remote",
		    ts_appmgr_pending_active() ? "yes" : "no");
#ifdef CONFIG_TS_NET
#include <ts/net.h>
	shell_print(sh, "[tsi] info net=%s", ts_net_link_up() ? "up" : "down");
#endif
	return 0;
}

static int cmd_ts_log(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(sh, "[tsi] log usage: ts log <off|err|wrn|inf|dbg>");
		return -EINVAL;
	}
	ts_res_t rc = ts_interact_log_level(argv[1]);

	shell_print(sh, "[tsi] log %s rc=%d", argv[1], (int)rc);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ts,
	SHELL_CMD(pending, NULL, "show pending install", cmd_ts_pending),
	SHELL_CMD(confirm, NULL, "confirm pending install (F3 = BOOT short)",
		  cmd_ts_confirm),
	SHELL_CMD(reject, NULL, "reject pending install (F3 = BOOT long)",
		  cmd_ts_reject),
	SHELL_CMD(arb, NULL, "safety arb seat (CG3 = BOOT double-short)",
		  cmd_ts_arb),
	SHELL_CMD(estop, NULL, "local estop clear (F2)", cmd_ts_estop),
	SHELL_CMD(info, NULL, "state overview", cmd_ts_info),
	SHELL_CMD(log, NULL, "log verbosity", cmd_ts_log),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ts, &sub_ts, "Tessera local interaction", NULL);
#endif /* CONFIG_SHELL */
