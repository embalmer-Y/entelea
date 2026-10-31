/* SPDX-License-Identifier: Apache-2.0 */
/* 包接收/验签/写 slot（LLD-ts-appmgr §2/§3）。
 * 验签（DEC-49，Q-29④）= verify.c 全量管线：TSAP v2 头 + sha256 摘要对拍 +
 * COSE_Sign1 ed25519 真验签（tweetnacl；TEST 构建 = 固定测试根，生产 = prov
 * pk0）——IR2-02 结构桩时代结束；失败 → TS_E_INVALID_SIG + 留痕。
 * MA3.1：安装链分步化（stage_begin/chunk/verify/activate，LLD-agent-deploy-net §3）——
 * ts_appmgr_install 与 sys/app-* 命令消费同一内部链（行为一致）。 */
#include <stdio.h>
#include <string.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/store.h>
#include <ts/tsap.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "internal.h"
#include "../net/internal.h" /* ts_cbor_* 读助手（tessera 模块内复用） */

/* ---- 资源档位扫描与安装期匹配（CG4 · Q-39②⑤/Q-42 · LLD §5a）------------ */
#ifdef CONFIG_TS_APP_WAMR

ts_res_t ts_appmgr_scan_needs(const uint8_t *man, uint32_t ml, ts_app_needs_t *out)
{
	if (man == NULL || out == NULL) {
		return TS_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
#ifdef CONFIG_TS_HAL_FS
	out->res_csv[0] = '\0';
#endif
	ts_cbor_rd_t r;
	uint32_t pairs;

	ts_cbor_rd_init(&r, man, ml);
	if (!ts_cbor_map_open(&r, &pairs)) {
		return TS_E_PARAM;
	}
	for (uint32_t i = 0; i < pairs; i++) {
		char key[16];

		if (!ts_cbor_tstr(&r, key, sizeof(key))) {
			return TS_E_PARAM;
		}
		if (strcmp(key, "tier") == 0) {
			char t[8];

			if (!ts_cbor_tstr(&r, t, sizeof(t))) {
				return TS_E_PARAM;
			}
			if (strcmp(t, "small") == 0) {
				out->tier = 0;
			} else if (strcmp(t, "large") == 0) {
				out->tier = 1;
			} else {
				return TS_E_PARAM; /* 未知档位 fail-closed */
			}
		} else if (strcmp(key, "needs") == 0) {
			out->has_needs = true;
			uint32_t np;

			if (!ts_cbor_map_open(&r, &np)) {
				return TS_E_PARAM;
			}
			for (uint32_t j = 0; j < np; j++) {
				char nk[20];

				if (!ts_cbor_tstr(&r, nk, sizeof(nk))) {
					return TS_E_PARAM;
				}
				if (strcmp(nk, "linear_memory_kb") == 0 ||
				    strcmp(nk, "stack_kb") == 0) {
					uint64_t v;

					if (!ts_cbor_uint(&r, &v) || v == 0 ||
					    v > 65535) {
						return TS_E_PARAM;
					}
					if (nk[0] == 'l') {
						out->linear_memory_kb = (uint32_t)v;
					} else {
						out->stack_kb = (uint32_t)v;
					}
				} else if (strcmp(nk, "resources") == 0) {
					uint32_t n;

					if (!ts_cbor_array_open(&r, &n) || n > 16) {
						return TS_E_PARAM;
					}
					for (uint32_t k = 0; k < n; k++) {
						char pfx[68]; /* fs_paths 同文法（agent 条目上限 64+NUL） */

						if (!ts_cbor_tstr(&r, pfx, sizeof(pfx)) ||
						    pfx[0] != '/' ||
						    strstr(pfx, "..") != NULL) {
							return TS_E_PARAM;
						}
#ifdef CONFIG_TS_HAL_FS
						/* CG5-B：前缀累积 CSV（res registry 数据源；
						 * 容量 = CONFIG_TS_HAL_FS_PATHS_MAX，超限拒绝） */
						size_t pl = strlen(pfx);
						size_t cur = strlen(out->res_csv);

						if (cur + pl + 2 >= sizeof(out->res_csv)) {
							return TS_E_PARAM;
						}
						if (cur > 0) {
							out->res_csv[cur++] = ';';
						}
						memcpy(out->res_csv + cur, pfx, pl + 1);
#endif
					}
					out->resources = (uint16_t)n;
				} else {
					return TS_E_PARAM; /* needs 内未知键 fail-closed */
				}
			}
		} else if (!ts_cbor_skip(&r, 0)) {
			return TS_E_PARAM; /* 非目标键跳过失败 = 结构非法 */
		}
	}
	/* schema 纪律：needs 仅 large 可携（small 档资源面 = fs_paths 既有语义） */
	if (out->has_needs && out->tier == 0) {
		return TS_E_PARAM;
	}
	return TS_OK;
}

ts_res_t ts_appmgr_needs_match(const ts_app_needs_t *n)
{
	if (n == NULL) {
		return TS_E_PARAM;
	}
	/* ① 线性内存预算 ≤ 设备帽（WAMR 实例堆池推导——V1 单实例账已知限制） */
	if (n->linear_memory_kb != 0 &&
	    (uint64_t)n->linear_memory_kb * 1024 > (uint64_t)CONFIG_TS_APP_WAMR_HEAP) {
		printk("[appmgr] match: linear_memory_kb=%u > heap cap %u KB\n",
		       n->linear_memory_kb, CONFIG_TS_APP_WAMR_HEAP / 1024);
		return TS_E_RANGE;
	}
	/* ② 资源清单须 fs 能力（物理存在性 = 运行期 fs 挂载面，非安装期） */
#ifndef CONFIG_TS_HAL_FS
	if (n->resources > 0) {
		printk("[appmgr] match: resources=%u but TS_HAL_FS=n\n", n->resources);
		return TS_E_RANGE;
	}
#endif
	/* ③ 档位支持：设备 large 能力 = 装载缓冲 ≥ large 门（派生，无独立开关） */
	if (n->tier == 1 && CONFIG_TS_APP_LOAD_MAX < CONFIG_TS_APP_SIZE_GATE_LARGE) {
		printk("[appmgr] match: tier=large unsupported (LOAD_MAX=%d < gate=%d)\n",
		       CONFIG_TS_APP_LOAD_MAX, CONFIG_TS_APP_SIZE_GATE_LARGE);
		return TS_E_RANGE;
	}
	return TS_OK;
}

#endif /* CONFIG_TS_APP_WAMR */

/* ---- 分步安装 staging 状态（内存；begin 可重入重置）---------------------- */

static struct {
	bool active;
	bool verified;
	uint32_t total;
	uint32_t high_water; /* 已写最大 offset+len（断点续传进度） */
	uint8_t slot;        /* inactive slot（绝对槽号） */
	uint8_t entry;       /* 目标 entry（Q-32②；legacy 单 APP = 0） */
} stage;

/* ---- F3 安装闸挂起态（DEC-62① · Q-46①③）----------------------------------
 * pending 仅内存（复位即失 = fail-closed：无 meta 足迹、槽字节保留）；
 * 60s 超时 = k_work（sysworkq）撤销 + 事件。确认/拒绝仅固件内本地路。
 *（非 static——internal.h 声明，slot.c get_info 消费） */
struct ts_appmgr_pend pending;

static struct k_work_delayable pend_timer;
static ts_pending_evt_t pend_pl; /* 事件 payload 静态存储（core.h 公共类型） */

static void pend_publish(ts_evt_id_t id)
{
	pend_pl.slot = pending.slot;
	pend_pl.entry = pending.entry;
	const ts_evt_t evt = {.id = id, .t_ms = ts_time_ms(),
			      .data = &pend_pl, .len = sizeof(pend_pl)};
	ts_evt_publish(&evt);
}

static void pend_clear(void)
{
	pending.active = false;
	k_work_cancel_delayable(&pend_timer);
}

static void pend_timeout(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!pending.active) {
		return;
	}
	/* fail-closed：超时撤销（槽字节保留可重推；staging 会话终止） */
	pend_publish(TS_EVT_INSTALL_TIMEOUT);
	printk("[appmgr] pending install timeout slot=%u entry=%u "
	       "(cancelled — slot kept, re-push to retry)\n",
	       pending.slot, pending.entry);
	pend_clear();
	memset(&stage, 0, sizeof(stage));
}

ts_res_t ts_appmgr_stage_begin_n(uint32_t total_len, uint8_t entry, uint8_t *slot_out)
{
	/* v2 容器下限：头 16 + 摘要 32 + 最小 manifest/COSE + 签名域 */
	if (total_len < TSAP_CONTENT_OFF + 8 + 18 + 64 ||
	    total_len > CONFIG_TS_STORE_SLOT_SIZE ||
	    entry >= CONFIG_TS_APP_MAX_INSTANCES) {
		return TS_E_PARAM;
	}
	ts_appmgr_meta_t meta;

	ts_res_t mr = ts_appmgr_meta_read(&meta);

	if (mr != TS_OK) {
		/* meta 不可读（空板首装 / 双副本皆损——store 层如实 E_IO）：
		 * 显式恢复安装（目标 slot 0），printk 可观测（IR2-07：不静默） */
		printk("[appmgr] meta unreadable (r=%d) — recovery install to slot 0\n",
		       (int)mr);
		ts_appmgr_meta_default(&meta);
	}
	/* 目标槽 = entry 对内乒乓（绝对槽号 ^1）。entry 超出现有 count = 新
	 * APP 位（默认 active = 对基 2e → 目标 2e+1）；存量 entry 的
	 * active_slot 越对（损坏面）= 回退对基（fail-safe，不静默扩散）。 */
	uint8_t base = (uint8_t)(2 * entry);

	if (meta.count > entry &&
	    (meta.entries[entry].active_slot >> 1) == entry) {
		base = meta.entries[entry].active_slot;
	}
	/* flash 后端：安装前抹除目标 slot（物理只可 1→0；RAM 后端 = 0xFF 复位）。
	 * begin 可重入重置 = 重装同槽亦经此处（重置语义保持）。 */
	uint8_t target = base ^ 1;
	ts_res_t er = ts_store_slot_erase(target);

	if (er != TS_OK) {
		memset(&stage, 0, sizeof(stage));
		return er; /* 槽抹除失败 = 拒绝开始安装（fail-closed） */
	}
	memset(&stage, 0, sizeof(stage));
	pend_clear(); /* F3：新推覆盖闸（begin 重置语义——挂起窗随新会话终止） */
	stage.active = true;
	stage.total = total_len;
	stage.slot = target;
	stage.entry = entry;
	if (slot_out != NULL) {
		*slot_out = stage.slot;
	}
	return TS_OK;
}

ts_res_t ts_appmgr_stage_begin(uint32_t total_len, uint8_t *slot_out)
{
	return ts_appmgr_stage_begin_n(total_len, 0, slot_out);
}

ts_res_t ts_appmgr_stage_chunk(uint32_t off, const uint8_t *data, uint32_t len,
			       uint32_t *high_water)
{
	if (!stage.active || data == NULL || len == 0) {
		return TS_E_STATE;
	}
	if ((uint64_t)off + len > stage.total) {
		return TS_E_PARAM; /* 越界 = 拒绝（不静默截断） */
	}
	/* 逐块写 + 回读校验语义由 ts_store_slot_write 内置（LLD-ts-store §6）；
	 * 同 offset 重写幂等（重发安全，DEC-40 idem 之外的传输层兜底）。 */
	ts_res_t r = ts_store_slot_write(stage.slot, off, data, len);

	if (r != TS_OK) {
		return r;
	}
	if (off + len > stage.high_water) {
		stage.high_water = off + len;
	}
	if (high_water != NULL) {
		*high_water = stage.high_water;
	}
	return TS_OK;
}

ts_res_t ts_appmgr_stage_verify(const uint8_t root_pubkey[32],
				uint32_t *manifest_len, uint32_t *wasm_len,
				uint32_t *cose_off)
{
	if (!stage.active || stage.high_water < stage.total) {
		return TS_E_STATE; /* 未收满不得验（fail-closed） */
	}
	/* 1-3. 全量验签（verify.c：TSAP v2 头 → sha256 摘要对拍 → COSE ed25519；
	 * 读与校验全部以 slot 持久化面为准）。 */
	tsap_view_t v;
	ts_res_t r = ts_appmgr_verify_slot(stage.slot, root_pubkey, &v, NULL);

	if (r != TS_OK) {
		const ts_evt_t evt = {.id = TS_EVT_PERM_DENIED, .t_ms = ts_time_ms()};

		ts_evt_publish(&evt);
		return r;
	}
	if ((uint64_t)v.cose_off + 90 > stage.total) {
		return TS_E_PARAM; /* COSE 段不完整（< phdr+map0+payload 头+sig64） */
	}
	/* 4. manifest 边界（与 install 同标） */
	if (v.manifest_len < 8 || v.manifest_len > 512) {
		return TS_E_PARAM;
	}
#ifdef CONFIG_TS_APP_WAMR
	/* 4b. 档位扫描 + 安装期匹配矩阵（CG4 · Q-39⑤ · LLD §5a：验签后、激活前
	 * ——安装期拒装替代运行期 OOM。分步安装语义：包已入非激活槽，拒绝 =
	 * 不激活——等价"槽写入前拒"的净效果且免二次传输）。 */
	{
		static uint8_t man[512];
		ts_app_needs_t needs;

		if (ts_store_slot_read(stage.slot, TSAP_CONTENT_OFF, man,
				       v.manifest_len) != TS_OK) {
			return TS_E_IO;
		}
		ts_res_t sr = ts_appmgr_scan_needs(man, v.manifest_len, &needs);

		if (sr != TS_OK) {
			return sr; /* tier/needs schema 违例 fail-closed */
		}
		sr = ts_appmgr_needs_match(&needs);
		if (sr != TS_OK) {
			return sr; /* 匹配矩阵拒装（TS_E_RANGE + printk 缺口归因） */
		}
	}
#endif
	stage.verified = true;
	if (manifest_len != NULL) {
		*manifest_len = v.manifest_len;
	}
	if (wasm_len != NULL) {
		*wasm_len = v.wasm_len;
	}
	if (cose_off != NULL) {
		*cose_off = v.cose_off;
	}
	return TS_OK;
}

/* 提交体（activate 原步骤 5-7；F3 确认路复用——digest 复查 + 停旧 APP +
 * meta 原子切换 + 资源绑定 + 清台）。调用前置条件：stage.active/verified
 * 且 entry 匹配（两路共用同一前置检查）。 */
static ts_res_t stage_commit(uint8_t entry, ts_app_info_t *out)
{
	/* 5. 激活对拍（IR2-02 次账收口）：摘要重哈希 == 头摘要（verify 后
	 * 的持久化完整性复查）+ 摘要入 meta（boot 快校验对象）。 */
	tsap_view_t v;
	uint8_t digest[TSAP_DIGEST_SIZE];
	/* R5-V③ verdict 行保真：包身份随激活信息面（app_id/app_ver——安装期
	 * verify 已全 schema 走查，此处仅身份提取：未知键 skip 不复核）。 */
	char id_app[64] = "";
	char id_ver[64] = "";

	ts_res_t r = ts_appmgr_check_digest(stage.slot, &v);

	if (r != TS_OK) {
		return r;
	}
	{
		static uint8_t man_id[512];

		if (v.manifest_len >= 8 && v.manifest_len <= sizeof(man_id) &&
		    ts_store_slot_read(stage.slot, TSAP_CONTENT_OFF, man_id,
				       v.manifest_len) == TS_OK) {
			ts_cbor_rd_t rd;
			uint32_t pairs;

			ts_cbor_rd_init(&rd, man_id, v.manifest_len);
			if (ts_cbor_map_open(&rd, &pairs)) {
				for (uint32_t i = 0; i < pairs; i++) {
					char key[16];

					if (!ts_cbor_tstr(&rd, key, sizeof(key))) {
						break;
					}
					if (strcmp(key, "app_id") == 0 ||
					    strcmp(key, "app_ver") == 0) {
						char val[64];

						if (!ts_cbor_tstr(&rd, val, sizeof(val))) {
							break;
						}
						char *dst = (key[4] == 'i') ? id_app : id_ver;

						strncpy(dst, val, sizeof(id_app) - 1);
					} else if (!ts_cbor_skip(&rd, 0)) {
						break;
					}
				}
			}
		}
	}
	if (ts_store_slot_read(stage.slot, TSAP_DIGEST_OFF, digest,
			       TSAP_DIGEST_SIZE) != TS_OK) {
		return TS_E_IO;
	}
	/* G4（单元 H）+ M7（Q-32⑤）：激活 = 停**同 entry** 运行 APP（同 app_id
	 * 替换即停——不同 entry 互不影响；legacy 单实例语义等价）。旧缺陷：
	 * 翻转 meta 后旧包继续运行 = 撕裂态（deploybench DB4 workaround 时代）。
	 * 卡死（E_TIMEOUT）：生产照常翻转（群组 WDT + 健康探针处置）；
	 * TEST 如实失败返回（无复位面）。
	 * 门控：停旧面在 runtime.c（TS_APP_WAMR 门）——WAMR=n 的 store-only
	 * 构建无运行 APP 可停（netbench 类最小档；链接期未定义引用 2026-10-19
	 * MP 基线构建暴露）。 */
#ifdef CONFIG_TS_APP_WAMR
	if (ts_appmgr_app_running_id((uint16_t)(entry + 1))) {
		ts_res_t sr = ts_appmgr_app_stop_id((uint16_t)(entry + 1));

		if (sr != TS_OK) {
			printk("[appmgr] activate[%u]: app_stop r=%d（stuck — abandon to "
			       "group WDT）\n", entry, (int)sr);
#ifdef CONFIG_TS_TEST
			return sr;
#endif
		}
	}
#endif /* CONFIG_TS_APP_WAMR */
	/* 6. meta 原子切换（entry active_slot = staging slot + 摘要随载；
	 * 新 entry 位 = count 扩展） */
	ts_appmgr_meta_t meta;
	ts_res_t mr = ts_appmgr_meta_read(&meta);

	if (mr != TS_OK) {
		ts_appmgr_meta_default(&meta); /* 恢复安装路径（begin 已 printk） */
	}
	if (meta.count < entry + 1) {
		meta.count = entry + 1;
	}
	meta.entries[entry].active_slot = stage.slot;
	meta.entries[entry].rollback_count = 0; /* 新安装重置回滚计数 */
	memcpy(meta.entries[entry].content_digest, digest, TSAP_DIGEST_SIZE);
	r = ts_appmgr_meta_write(&meta);
	if (r != TS_OK) {
		return r;
	}
	/* 7. 更新运行时信息（与 install 一致） */
	memset(&current_apps[entry], 0, sizeof(current_apps[entry]));
	current_apps[entry].state = TS_APP_STAGED;
	current_apps[entry].active_slot = stage.slot;
	current_apps[entry].rollback_count = 0;
	current_apps[entry].pending_slot = 0xFF;
	strncpy(current_apps[entry].app_id, id_app,
		sizeof(current_apps[entry].app_id) - 1);
	strncpy(current_apps[entry].app_ver, id_ver,
		sizeof(current_apps[entry].app_ver) - 1);
	if (out != NULL) {
		*out = current_apps[entry];
	}
	initialized = true;
	const ts_evt_t evt = {.id = TS_EVT_APP_LOADED, .t_ms = ts_time_ms()};
	ts_evt_publish(&evt);
#if defined(CONFIG_TS_APP_WAMR) && defined(CONFIG_TS_HAL_FS)
	/* CG5-B（方案 A）：资源白名单随激活绑定（部署会话在激活后经 sys/fs-*
	 * 推送 needs.resources 前缀下的资源文件；无 needs = 清除——覆盖式）。
	 * 解析与安装期/boot 同源（scan_needs 单一事实源）。 */
	{
		static uint8_t man_act[512];
		ts_app_needs_t needs;

		if (ts_store_slot_read(stage.slot, TSAP_CONTENT_OFF, man_act,
				       v.manifest_len) == TS_OK &&
		    ts_appmgr_scan_needs(man_act, v.manifest_len, &needs) == TS_OK) {
			ts_appmgr_res_bind(entry + 1, needs.res_csv);
		} else {
			ts_appmgr_res_bind(entry + 1, ""); /* 读/扫描失败 = 清除（fail-closed 侧） */
		}
	}
#endif
	memset(&stage, 0, sizeof(stage)); /* 终态清台（下轮 begin 重置） */
	return TS_OK;
}

ts_res_t ts_appmgr_stage_activate_n(uint8_t entry, ts_app_info_t *out)
{
	if (!stage.active || !stage.verified || entry != stage.entry) {
		return TS_E_STATE; /* 未验证不得激活（fail-closed）；entry 与 begin 不符 = 拒绝 */
	}
	/* F3 安装闸（DEC-62①）：lconf=1 = activate 过闸不切 meta——置 pending
	 * （60s fail-closed）+ 事件外发；确认/拒绝仅固件内本地路（无 zenoh
	 * 命令面——远程不可确认，只能重推或等超时）。返回 TS_E_PENDING 结构化
	 * 中间态（net 命令层据此回执 pending 面）。 */
	if (ts_store_prov()->local_confirm) {
		if (pending.active) {
			return TS_E_BUSY; /* 单挂起窗（新推须先超时/被拒） */
		}
		pending.active = true;
		pending.slot = stage.slot;
		pending.entry = stage.entry;
		k_work_init_delayable(&pend_timer, pend_timeout);
		k_work_reschedule(&pend_timer,
				  K_MSEC(CONFIG_TS_APPMGR_PENDING_TIMEOUT_MS));
		pend_publish(TS_EVT_PENDING_INSTALL);
		printk("[appmgr] activate gated: PENDING slot=%u entry=%u "
		       "(local confirm, %ums fail-closed)\n",
		       pending.slot, pending.entry,
		       (unsigned)CONFIG_TS_APPMGR_PENDING_TIMEOUT_MS);
		if (out != NULL) {
			*out = current_apps[entry];
			out->pending_slot = pending.slot;
		}
		return TS_E_PENDING;
	}
	return stage_commit(entry, out);
}

/* ---- 安装闸本地路（F3 · DEC-62①）---------------------------------------- */

ts_res_t ts_appmgr_confirm_pending(ts_app_info_t *out)
{
	if (!pending.active) {
		return TS_E_STATE;
	}
	if (!stage.active || !stage.verified || stage.entry != pending.entry) {
		/* 防御：pending 期 stage 被清（理论不可达——begin/超时都先清
		 * pending；fail-closed 侧如实撤销） */
		pend_publish(TS_EVT_INSTALL_REJECTED);
		pend_clear();
		memset(&stage, 0, sizeof(stage));
		return TS_E_STATE;
	}
	uint8_t entry = pending.entry;
	ts_res_t r = stage_commit(entry, out);

	if (r == TS_OK) {
		pend_publish(TS_EVT_INSTALL_CONFIRMED);
		pend_clear();
	}
	return r;
}

ts_res_t ts_appmgr_reject_pending(void)
{
	if (!pending.active) {
		return TS_E_STATE;
	}
	pend_publish(TS_EVT_INSTALL_REJECTED);
	printk("[appmgr] pending install rejected slot=%u entry=%u "
	       "(cancelled — slot kept, re-push to retry)\n",
	       pending.slot, pending.entry);
	pend_clear();
	memset(&stage, 0, sizeof(stage)); /* 清台（远程重试须完整重推） */
	return TS_OK;
}

bool ts_appmgr_pending_active(void)
{
	return pending.active;
}

#ifdef CONFIG_TS_TEST
/* 测试隔离（F3）：pending/连带 staging 会话随 appmgr reset 清台（残留
 * 挂起定时器会跨用例误触发） */
void ts_appmgr_pend_reset_test(void)
{
	pend_clear();
	memset(&stage, 0, sizeof(stage));
}
#endif

int ts_appmgr_pending_summary(char *buf, size_t cap)
{
	if (!pending.active || buf == NULL || cap < 64) {
		return 0;
	}
	/* 呈现面单一事实源（EC3 §3.2）：manifest 数据面 = 固件解析（与
	 * Agent tsap_tools.manifest_summary 同字段族——ASCII 行，\n 分隔） */
	static uint8_t man[512];
	uint32_t man_len = 0;

	if (ts_store_slot_read(pending.slot, TSAP_CONTENT_OFF, man, sizeof(man))
		    != TS_OK) {
		return 0;
	}
	/* manifest 长度自 TSAP 头（check_digest 的 view 同源——直读头） */
	tsap_view_t v;

	if (ts_appmgr_check_digest(pending.slot, &v) != TS_OK) {
		return 0;
	}
	man_len = v.manifest_len;

	char app_id[64] = "?";
	char app_ver[16] = "?";
	char caps[160] = "-";
	char tier[8] = "small";
	char res[96] = "-";
	unsigned threads = 1;

#if defined(CONFIG_TS_APP_WAMR) && defined(CONFIG_TS_HAL_FS)
	{
		ts_app_needs_t nd;

		if (ts_appmgr_scan_needs(man, man_len, &nd) == TS_OK) {
			strncpy(tier, nd.tier == 1 ? "large" : "small", sizeof(tier) - 1);
			strncpy(res, nd.res_csv[0] ? nd.res_csv : "-", sizeof(res) - 1);
			res[sizeof(res) - 1] = '\0';
		}
	}
#endif
	if (man_len <= sizeof(man)) {
		ts_cbor_rd_t r;
		uint32_t pairs;

		ts_cbor_rd_init(&r, man, man_len);
		if (ts_cbor_map_open(&r, &pairs)) {
			for (uint32_t i = 0; i < pairs; i++) {
				char key[16];

				if (!ts_cbor_tstr(&r, key, sizeof(key))) {
					break;
				}
				if (strcmp(key, "app_id") == 0) {
					ts_cbor_tstr(&r, app_id, sizeof(app_id));
				} else if (strcmp(key, "app_ver") == 0) {
					ts_cbor_tstr(&r, app_ver, sizeof(app_ver));
				} else if (strcmp(key, "threads") == 0) {
					uint64_t u;

					if (ts_cbor_uint(&r, &u)) {
						threads = (unsigned)u;
					}
				} else if (strcmp(key, "caps") == 0) {
					uint32_t n;

					if (ts_cbor_array_open(&r, &n)) {
						size_t cl = 0;

						caps[0] = '\0';
						for (uint32_t j = 0; j < n; j++) {
							char cap[48];

							if (!ts_cbor_tstr(&r, cap, sizeof(cap)) ||
							    cl + strlen(cap) + 2 >= sizeof(caps)) {
								break;
							}
							if (cl > 0) {
								caps[cl++] = ';';
							}
							memcpy(caps + cl, cap, strlen(cap));
							cl += strlen(cap);
							caps[cl] = '\0';
						}
					}
				} else if (!ts_cbor_skip(&r, 1)) {
					break; /* tier/needs 等：scan_needs 单一事实源已取 */
				}
			}
		}
	}
	int n = snprintf(buf, cap,
			 "app: %s %s\n"
			 "slot: %u  entry: %u\n"
			 "tier: %s  threads: %u\n"
			 "caps: %s\n"
			 "res: %s\n"
			 "wasm: %uB  pkg: %uB",
			 app_id, app_ver, pending.slot, pending.entry, tier,
			 threads, caps, res, (unsigned)v.wasm_len,
			 (unsigned)stage.total);

	return (n > 0 && (size_t)n < cap) ? n : 0;
}

ts_res_t ts_appmgr_stage_activate(ts_app_info_t *out)
{
	return ts_appmgr_stage_activate_n(0, out);
}

/* ---- 整包一次式安装（本地路径；= 分步链的组合，行为与 M2b.1 一致）-------- */

ts_res_t ts_appmgr_install(const uint8_t *pkg_data, size_t pkg_len,
			    const uint8_t root_pubkey[32], ts_app_info_t *out)
{
	/* EC2（Q-43⑤）：root_pubkey 可为 NULL = 有效信任集（verify_slot 语义；
	 * 显式键 = 单键验签维持）。 */
	if (pkg_data == NULL || out == NULL) {
		return TS_E_PARAM;
	}
	if (ts_appmgr_stage_begin((uint32_t)pkg_len, NULL) != TS_OK) {
		return TS_E_INVALID_SIG; /* 尺寸非法 = 容器非法（与旧语义一致） */
	}
	ts_res_t r = ts_appmgr_stage_chunk(0, pkg_data, (uint32_t)pkg_len, NULL);

	if (r != TS_OK) {
		memset(&stage, 0, sizeof(stage));
		return r;
	}
	uint32_t ml = 0, wl = 0, co = 0;

	r = ts_appmgr_stage_verify(root_pubkey, &ml, &wl, &co);
	if (r != TS_OK) {
		memset(&stage, 0, sizeof(stage));
		return r;
	}
	return ts_appmgr_stage_activate(out);
}
