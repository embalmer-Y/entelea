/* SPDX-License-Identifier: Apache-2.0 */
/* 槽池 + meta v2 管理 + 生命周期状态机（LLD-ts-appmgr §3/§4；Q-32②/DEC-53）。
 * meta 经 ts-store 掉电安全 kv（LLD-ts-store §3）；回滚计数超限 → QUARANTINED
 * （per entry）。entry e 槽对 = {2e, 2e+1}（绝对槽号，^1 = 对内乒乓翻转）。 */
#include <string.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/store.h>
#include <ts/tsap.h>
#include <zephyr/kernel.h>
#if !defined(CONFIG_TS_TEST) && defined(CONFIG_REBOOT)
#include <zephyr/sys/reboot.h> /* IR2-07：回滚后暖复位重载目标槽 */
#endif
#include "internal.h" /* ts_appmgr_verify_slot/check_digest（DEC-49） */
#if defined(CONFIG_TS_HAL_FS) || defined(CONFIG_TS_HAL_AV)
#include <ts/hal.h> /* ts_fs_paths_bind / ts_av_config_bind（DEC-47⑤③） */
#endif

/* Q-32①②编译守卫：槽池容量须容纳 每 entry 双槽乒乓 */
_Static_assert(2 * CONFIG_TS_APP_MAX_INSTANCES <= CONFIG_TS_STORE_SLOT_COUNT,
	       "slot pool too small: 2*TS_APP_MAX_INSTANCES <= TS_STORE_SLOT_COUNT (Q-32)");

ts_app_info_t current_apps[CONFIG_TS_APP_MAX_INSTANCES];
bool initialized;

/* ---- 宿主资源推送白名单 registry（CG5-B · Q-39③ 方案 A）------------------
 * 随包联动：activate 绑定 + boot 恢复（needs.resources → scan res_csv）；
 * sys/fs-* 命令面授权源。匹配语义 = ts_fs_prefix_match（边界纪律单一事实源）。 */
#if defined(CONFIG_TS_APP_WAMR) && defined(CONFIG_TS_HAL_FS)
static char res_paths[CONFIG_TS_APP_MAX_INSTANCES][CONFIG_TS_HAL_FS_PATHS_MAX];

void ts_appmgr_res_bind(uint8_t app_id, const char *csv)
{
	if (app_id == 0 || app_id > CONFIG_TS_APP_MAX_INSTANCES || csv == NULL) {
		return;
	}
	strncpy(res_paths[app_id - 1], csv, sizeof(res_paths[0]) - 1);
	res_paths[app_id - 1][sizeof(res_paths[0]) - 1] = '\0';
}

ts_res_t ts_appmgr_res_allows(uint8_t app_id, const char *path)
{
	if (app_id == 0 || app_id > CONFIG_TS_APP_MAX_INSTANCES || path == NULL) {
		return TS_E_PARAM;
	}
	return ts_fs_prefix_match(res_paths[app_id - 1], path) ? TS_OK : TS_E_PERM;
}
#endif

#ifdef CONFIG_TS_TEST
void ts_appmgr_test_reset(void)
{
	initialized = false;
	memset(current_apps, 0, sizeof(current_apps));
#if defined(CONFIG_TS_APP_WAMR) && defined(CONFIG_TS_HAL_FS)
	memset(res_paths, 0, sizeof(res_paths));
#endif
	ts_appmgr_pend_reset_test(); /* F3：挂起定时器/staging 随隔离清台 */
}
#endif

/* v1 meta 布局（M6 前单条 40B——长度判别迁移用；与 entry 同构） */
typedef struct {
	uint8_t active_slot;
	uint8_t rollback_count;
	uint16_t boot_gen;
	uint32_t app_ver_u32;
	uint8_t content_digest[32];
} meta_v1_t;

void ts_appmgr_meta_default(ts_appmgr_meta_t *meta)
{
	memset(meta, 0, sizeof(*meta));
	meta->ver = TS_APPMGR_META_VER;
	meta->count = 1;
	meta->entries[0].active_slot = 0;
}

ts_res_t ts_appmgr_meta_read(ts_appmgr_meta_t *meta)
{
	if (meta == NULL) return TS_E_PARAM;
	uint16_t len = 0;

	if (ts_store_meta_read(meta, &len) == TS_OK) {
		if (len == sizeof(meta_v1_t)) {
			/* v1 → v2 迁移（同构直拷 entries[0]；下次写持久化 v2） */
			meta_v1_t v1;

			memcpy(&v1, meta, sizeof(v1));
			ts_appmgr_meta_default(meta);
			meta->entries[0].active_slot = v1.active_slot;
			meta->entries[0].rollback_count = v1.rollback_count;
			meta->entries[0].boot_gen = v1.boot_gen;
			meta->entries[0].app_ver_u32 = v1.app_ver_u32;
			memcpy(meta->entries[0].content_digest, v1.content_digest,
			       sizeof(v1.content_digest));
			return TS_OK;
		}
		if (len == sizeof(*meta) && meta->ver == TS_APPMGR_META_VER &&
		    meta->count >= 1 && meta->count <= CONFIG_TS_APP_MAX_INSTANCES) {
			return TS_OK;
		}
	}
	/* IR2-07：空板首装与双副本皆损在此同象（store 层不可分辨）——
	 * 如实上报 E_IO，不静默伪造成有效 meta；调用方按各自语义处置
	 * （boot = 不装载；install = 显式恢复安装〔begin printk 可观测〕） */
	ts_appmgr_meta_default(meta);
	return TS_E_IO;
}

ts_res_t ts_appmgr_meta_write(const ts_appmgr_meta_t *meta)
{
	return ts_store_meta_write(meta, (uint16_t)sizeof(*meta));
}

ts_res_t ts_appmgr_get_info_n(uint8_t entry, ts_app_info_t *out)
{
	if (out == NULL || entry >= CONFIG_TS_APP_MAX_INSTANCES) {
		return TS_E_PARAM;
	}
	if (!initialized) {
		/* 首次调用：从 meta 恢复（掉电重启路径）；不可读（空板/双损）=
		 * 缺省可观测态（不静默写回——IR2-07） */
		ts_appmgr_meta_t meta;
		bool have = ts_appmgr_meta_read(&meta) == TS_OK;

		for (uint8_t e = 0; e < CONFIG_TS_APP_MAX_INSTANCES; e++) {
			if (have && e < meta.count) {
				current_apps[e].active_slot =
					meta.entries[e].active_slot;
				current_apps[e].rollback_count =
					meta.entries[e].rollback_count;
			}
			current_apps[e].state = TS_APP_STAGED;
		}
		initialized = true;
	}
	*out = current_apps[entry];
	/* F3 · DEC-62①：安装闸挂起面（加性——pending_slot 0xFF = 无） */
	out->pending_slot = (pending.active && pending.entry == entry)
		? pending.slot : 0xFF;
	return TS_OK;
}

ts_res_t ts_appmgr_get_info(ts_app_info_t *out)
{
	return ts_appmgr_get_info_n(0, out);
}

ts_res_t ts_appmgr_rollback_n(uint8_t entry)
{
	if (entry >= CONFIG_TS_APP_MAX_INSTANCES) return TS_E_PARAM;
	if (!initialized) return TS_E_STATE;
	ts_app_info_t *ca = &current_apps[entry];

	if (ca->state != TS_APP_ACTIVE && ca->state != TS_APP_ROLLBACK) {
		return TS_E_STATE;
	}
	if (ca->rollback_count >= TS_APPMGR_ROLLBACK_LIMIT) {
		/* DEC-27 #9：超限 → QUARANTINED（终态，不循环回滚） */
		ca->state = TS_APP_QUARANTINED;
		/* G4（单元 H）：隔离持久化——meta entry.rollback_count = LIMIT+1
		 * 为拒载标记（boot_start 判据）；写失败留痕（重启重走判定）。 */
		ts_appmgr_meta_t meta;

		if (ts_appmgr_meta_read(&meta) == TS_OK &&
		    meta.count > entry &&
		    meta.entries[entry].rollback_count <= TS_APPMGR_ROLLBACK_LIMIT) {
			meta.entries[entry].rollback_count = TS_APPMGR_ROLLBACK_LIMIT + 1;
			if (ts_appmgr_meta_write(&meta) != TS_OK) {
				printk("[appmgr] quarantine marker persist failed\n");
			}
		}
		const ts_evt_t evt = {.id = TS_EVT_APP_QUARANTINED, .t_ms = ts_time_ms()};
		ts_evt_publish(&evt);
		return TS_E_ROLLBACK_LIMIT;
	}
	/* IR2-07：回滚闭环——目标槽先全量验签（头 v2/摘要/ed25519），
	 * 空/坏槽 = 如实失败且不翻转 active_slot（对内乒乓 ^1）。 */
	uint8_t target = ca->active_slot ^ 1;
	tsap_view_t tv;
	ts_res_t vr = ts_appmgr_verify_slot(target, NULL, &tv, NULL);

	if (vr != TS_OK) {
		printk("[appmgr] rollback[%u]: target slot %u invalid (r=%d) — no flip\n",
		       entry, target, (int)vr);
		return TS_E_IO;
	}
	ts_appmgr_meta_t meta;
	ts_res_t mr = ts_appmgr_meta_read(&meta);

	if (mr != TS_OK || meta.count <= entry) {
		printk("[appmgr] rollback[%u]: meta unreadable (r=%d) — refused\n",
		       entry, (int)mr);
		return TS_E_IO;
	}
	/* 切换 slot + 递增计数 + 写 meta（原子；摘要随目标槽更新） */
	ts_app_state_t prev_state = ca->state;

	ca->active_slot = target;
	ca->rollback_count++;
	ca->state = TS_APP_ROLLBACK;
	meta.entries[entry].active_slot = target;
	meta.entries[entry].rollback_count = ca->rollback_count;
	if (ts_store_slot_read(target, TSAP_DIGEST_OFF,
			       meta.entries[entry].content_digest,
			       TSAP_DIGEST_SIZE) != TS_OK) {
		return TS_E_IO;
	}
	ts_res_t r = ts_appmgr_meta_write(&meta);

	if (r != TS_OK) {
		/* meta 写失败：回退运行时状态并报错——运行态与持久态不得分叉
		 * （impl-review IR-09；重启后回到旧 meta = 安全侧） */
		ca->active_slot ^= 1;
		ca->rollback_count--;
		ca->state = prev_state;
		return r;
	}
	const ts_evt_t evt = {.id = TS_EVT_APP_UNLOADED, .t_ms = ts_time_ms()};
	ts_evt_publish(&evt);
#if !defined(CONFIG_TS_TEST) && defined(CONFIG_REBOOT)
	/* IR2-07：回滚闭环——meta 翻转后暖复位装载目标槽。TEST 构建不复位
	 * （用例断言状态机）；无 REBOOT 的板配置 = 如实登记（printk 可观测）。 */
	printk("[appmgr] rollback[%u]: warm reboot to slot %u\n", entry, ca->active_slot);
	sys_reboot(SYS_REBOOT_WARM);
#elif !defined(CONFIG_TS_TEST)
	printk("[appmgr] rollback[%u]: slot flipped (no CONFIG_REBOOT — manual reload)\n",
	       entry);
#endif
	return TS_OK;
}

ts_res_t ts_appmgr_rollback(void)
{
	return ts_appmgr_rollback_n(0);
}

ts_res_t ts_appmgr_health_fail(void)
{
	return ts_appmgr_rollback_n(0);
}

ts_res_t ts_appmgr_health_fail_id(uint16_t app_id)
{
	/* M7 分账（Q-32⑤/DEC-53）：rt 健康探针 → 对应 entry 回滚链
	 * （app_id = entry+1；超界 = entry 0，防御面） */
	if (app_id == 0 || app_id > CONFIG_TS_APP_MAX_INSTANCES) {
		app_id = 1;
	}
	return ts_appmgr_rollback_n((uint8_t)(app_id - 1));
}

/* ---- boot 装载（M2b.2 收尾单元；HLD §4.4-8：APP 故障不阻塞启动）--------- */

#ifdef CONFIG_TS_APP_WAMR
#include "../net/internal.h" /* ts_cbor_* 读助手（tessera 模块内复用） */

/* K5（plan §11f · 大档板级配置）：装载缓冲可选 PSRAM 化——large 档
 * LOAD_MAX（≥256KB）在 DRAM 受限板不可行；镜像 TS_APP_PSRAM_HEAP 机制
 * （HLD §4.6 分层纪律：APP 沙箱域可入外部 RAM；教训 35 = 缓冲须可写，
 * PSRAM 满足）。懒分配一次常驻（boot 装载路径单线程）。 */
#if defined(CONFIG_TS_APP_PSRAM_LOAD_BUF) && defined(CONFIG_SHARED_MULTI_HEAP)
#include <zephyr/multi_heap/shared_multi_heap.h>
static uint8_t *boot_wasm_buf; /* SMH_REG_ATTR_EXTERNAL 懒分配 */
static uint8_t *boot_wasm_buf_get(void)
{
	if (boot_wasm_buf == NULL) {
		boot_wasm_buf = shared_multi_heap_alloc(
			SMH_REG_ATTR_EXTERNAL, CONFIG_TS_APP_LOAD_MAX);
		if (boot_wasm_buf != NULL) {
			printk("[appmgr] load buf: psram %uB\n",
			       CONFIG_TS_APP_LOAD_MAX);
		}
	}
	return boot_wasm_buf;
}
#define BOOT_WASM_BUF_SZ ((size_t)CONFIG_TS_APP_LOAD_MAX)
#else
static uint8_t boot_wasm_buf[CONFIG_TS_APP_LOAD_MAX];
static uint8_t *boot_wasm_buf_get(void)
{
	return boot_wasm_buf;
}
#define BOOT_WASM_BUF_SZ (sizeof(boot_wasm_buf))
#endif
/* 槽池结构装配检查（CG4 · LLD-ts-store §4 注：统一大槽 + 档位逻辑上限——
 * 装载缓冲不得超槽容量；large 档板 = LOAD_MAX/SLOT_SIZE 同步抬升） */
BUILD_ASSERT(CONFIG_TS_APP_LOAD_MAX <= CONFIG_TS_STORE_SLOT_SIZE,
	     "TS_APP_LOAD_MAX must stay <= TS_STORE_SLOT_SIZE");
static char boot_caps[160];
#ifdef CONFIG_TS_HAL_FS
static char boot_fs_paths[CONFIG_TS_HAL_FS_PATHS_MAX]; /* DEC-47⑤ */
#endif
#ifdef CONFIG_TS_HAL_AV
static ts_av_fmt_t boot_av_fmt; /* DEC-47③（MD1.2g）：采集格式/分辨率随载绑定 */
static uint16_t boot_av_w, boot_av_h;
static uint8_t boot_av_set; /* 位掩码：1=fmt 2=w 4=h（全置 = 声明完整） */
#endif

/* 单 entry 装载（meta entry e → 槽 meta.entries[e].active_slot）：
 * manifest 走查 + wasm 读出 + app_start。返回 app_start 结果（skip 类
 * 非致命——合同 6：APP 故障不阻塞启动〔亦不阻塞后续 entry〕）。 */
static ts_res_t boot_load_entry(uint8_t entry, const ts_appmgr_meta_t *meta)
{
	uint8_t slot = meta->entries[entry].active_slot;
	uint8_t hdr[TSAP_HEADER_SIZE];

	/* 静态采集态按 entry 复位（M7 多 entry 装载——跨 entry 泄漏防御；
	 * boot_caps/fs_paths 在 walk 内复位，av_set 此处统一） */
#ifdef CONFIG_TS_HAL_AV
	boot_av_set = 0;
#endif
	if (ts_store_slot_read(slot, 0, hdr, sizeof(hdr)) != TS_OK) {
		return TS_E_IO;
	}
	if (memcmp(hdr, "TSAP", 4) != 0) {
		return TS_E_NOTFOUND; /* 空/未写 slot = 无 APP（boot 不阻塞） */
	}
	/* v2 快校验（DEC-49②）：sha256(manifest‖wasm) == 头摘要（毫秒级——
	 * 全量 ed25519 验签在安装/激活期完成；boot 只防持久化位腐）。 */
	tsap_view_t v;

	if (ts_appmgr_check_digest(slot, &v) != TS_OK) {
		printk("[appmgr] boot[%u]: digest mismatch (slot %u) — no app loaded\n",
		       entry, slot);
		return TS_E_INVALID_SIG;
	}
	uint32_t ml = v.manifest_len;
	uint32_t wl = v.wasm_len;

	if (ml == 0 || ml > 512 || wl == 0 || wl > BOOT_WASM_BUF_SZ) {
		return TS_E_PARAM; /* 容器头越界（装载上限 = CONFIG_TS_APP_LOAD_MAX） */
	}
	static uint8_t man[512];

	if (ts_store_slot_read(slot, TSAP_CONTENT_OFF, man, ml) != TS_OK) {
		return TS_E_IO;
	}
	/* manifest 走查（TsapManifest v1 canonical CBOR；按键名分发——
	 * fail-closed：未知键拒绝）。caps 组合为 ';' 分隔串（runtime 逐条 parse，
	 * ts_perm_parse 语义 = 位或合并）。stack_kb/heap_kb V1 消耗运行时常量
	 * （LLD §7 已知限制——manifest 读取但暂不生效，板级内存预算批兑现）。 */
	ts_cbor_rd_t r;
	uint32_t pairs;

	ts_cbor_rd_init(&r, man, ml);
	if (!ts_cbor_map_open(&r, &pairs)) {
		return TS_E_PARAM;
	}
	boot_caps[0] = '\0';
	size_t caps_len = 0;

	for (uint32_t i = 0; i < pairs; i++) {
		char key[16];

		if (!ts_cbor_tstr(&r, key, sizeof(key))) {
			return TS_E_PARAM;
		}
		if (strcmp(key, "caps") == 0) {
			uint32_t n;

			if (!ts_cbor_array_open(&r, &n)) {
				return TS_E_PARAM;
			}
			for (uint32_t j = 0; j < n; j++) {
				char cap[48];

				if (!ts_cbor_tstr(&r, cap, sizeof(cap))) {
					return TS_E_PARAM;
				}
				size_t cl = strlen(cap);

				if (caps_len + cl + 2 >= sizeof(boot_caps)) {
					return TS_E_PARAM;
				}
				if (caps_len > 0) {
					boot_caps[caps_len++] = ';';
				}
				memcpy(boot_caps + caps_len, cap, cl);
				caps_len += cl;
				boot_caps[caps_len] = '\0';
			}
		} else if (strcmp(key, "app_id") == 0 || strcmp(key, "app_ver") == 0) {
			char val[64];

			if (!ts_cbor_tstr(&r, val, sizeof(val))) {
				return TS_E_PARAM;
			}
			if (strcmp(key, "app_id") == 0) {
				strncpy(current_apps[entry].app_id, val,
					sizeof(current_apps[entry].app_id) - 1);
				current_apps[entry].app_id[sizeof(current_apps[entry].app_id) - 1] = '\0';
			} else {
				strncpy(current_apps[entry].app_ver, val,
					sizeof(current_apps[entry].app_ver) - 1);
				current_apps[entry].app_ver[sizeof(current_apps[entry].app_ver) - 1] = '\0';
			}
		} else if (strcmp(key, "min_fw_ver") == 0) {
			char val[32];

			if (!ts_cbor_tstr(&r, val, sizeof(val))) {
				return TS_E_PARAM;
			}
		} else if (strcmp(key, "stack_kb") == 0 || strcmp(key, "heap_kb") == 0) {
			uint64_t v;

			if (!ts_cbor_uint(&r, &v)) {
				return TS_E_PARAM;
			}
			/* V1 消耗运行时常量（见上注释） */
		} else if (strcmp(key, "threads") == 0) {
			/* Q-33/DEC-53（V0.4.0 M8）：wasm 线程申报制（1-4；越界 = 包
			 * 非法 fail-closed）。DEC-55（Q-37 A，M19a）：TS_APP_THREADS
			 * 构建下 threads>1 = 真线程（宿主侧 spawn 编排，总并发含主）；
			 * 默认构建维持单线程如实留痕（DEC-31 语义）。 */
			uint64_t v;

			if (!ts_cbor_uint(&r, &v) || v < 1 || v > 4) {
				return TS_E_PARAM;
			}
#ifdef CONFIG_TS_APP_THREADS
			ts_appmgr_app_set_threads((uint16_t)(entry + 1),
						  (uint8_t)v);
			if (v > 1) {
				printk("[appmgr] boot[%u]: threads=%u "
				       "(multi-threading enabled)\n",
				       entry, (unsigned)v);
			}
#else
			if (v > 1) {
				printk("[appmgr] boot[%u]: threads=%u declared, "
				       "runtime single-threaded (TS_APP_THREADS=n)\n",
				       entry, (unsigned)v);
			}
#endif
#ifdef CONFIG_TS_HAL_FS
		} else if (strcmp(key, "fs_paths") == 0) {
			/* DEC-47⑤（MD1.2e）：路径前缀白名单（数组 → ';' CSV；
			 * fail-closed：越界/非绝对路径条目拒绝装载） */
			uint32_t n;

			if (!ts_cbor_array_open(&r, &n)) {
				return TS_E_PARAM;
			}
			boot_fs_paths[0] = '\0';
			size_t plen = 0;

			for (uint32_t j = 0; j < n; j++) {
				char pfx[68]; /* agent 校验器条目上限 64+NUL；68 对齐（复检发现②） */

				if (!ts_cbor_tstr(&r, pfx, sizeof(pfx)) ||
				    pfx[0] != '/' ||
				    strstr(pfx, "..") != NULL) {
					return TS_E_PARAM;
				}
				size_t l = strlen(pfx);

				if (plen + l + 2 >= sizeof(boot_fs_paths)) {
					return TS_E_PARAM;
				}
				if (plen > 0) {
					boot_fs_paths[plen++] = ';';
				}
				memcpy(boot_fs_paths + plen, pfx, l);
				plen += l;
				boot_fs_paths[plen] = '\0';
			}
#endif /* CONFIG_TS_HAL_FS */
#ifdef CONFIG_TS_HAL_AV
		} else if (strcmp(key, "av_fmt") == 0) {
			/* DEC-47③（MD1.2g）+ Q-36/DEC-54②：采集格式（jpeg/rgb565
			 * 图像；pcm16 音频 = mono 16kHz 16bit，帧 640B 定长） */
			char f[8];

			if (!ts_cbor_tstr(&r, f, sizeof(f))) {
				return TS_E_PARAM;
			}
			if (strcmp(f, "jpeg") == 0) {
				boot_av_fmt = TS_AV_FMT_JPEG;
			} else if (strcmp(f, "rgb565") == 0) {
				boot_av_fmt = TS_AV_FMT_RGB565;
			} else if (strcmp(f, "pcm16") == 0) {
				boot_av_fmt = TS_AV_FMT_PCM16;
			} else {
				return TS_E_PARAM;
			}
			boot_av_set |= 1U;
		} else if (strcmp(key, "av_w") == 0 || strcmp(key, "av_h") == 0) {
			uint64_t v;

			if (!ts_cbor_uint(&r, &v) || v < 16 || v > 800) {
				return TS_E_PARAM;
			}
			if (key[3] == 'w') {
				/* pcm16：w = 采样率 kHz（V1 = 16 硬校验） */
				if (boot_av_fmt == TS_AV_FMT_PCM16 && v != 16) {
					return TS_E_PARAM;
				}
				boot_av_w = (uint16_t)v;
				boot_av_set |= 2U;
			} else {
				if (boot_av_fmt == TS_AV_FMT_PCM16) {
					return TS_E_PARAM; /* pcm16 无 av_h（schema 纪律） */
				}
				boot_av_h = (uint16_t)v;
				boot_av_set |= 4U;
			}
#endif /* CONFIG_TS_HAL_AV */
		} else if (strcmp(key, "exports") == 0) {
			uint32_t n;

			if (!ts_cbor_array_open(&r, &n)) {
				return TS_E_PARAM;
			}
			for (uint32_t j = 0; j < n; j++) {
				char ex[48];

				if (!ts_cbor_tstr(&r, ex, sizeof(ex))) {
					return TS_E_PARAM;
				}
			}
		} else if (strcmp(key, "tier") == 0 || strcmp(key, "needs") == 0) {
			/* CG4（Q-39②/Q-42）：manifest v2 档位键——walk 接受并跳过；
			 * 语义解析统一走 ts_appmgr_scan_needs（单一事实源，walk 后
			 * 调用取档位门——与安装期 verify 同解析器） */
			if (!ts_cbor_skip(&r, 0)) {
				return TS_E_PARAM;
			}
		} else {
			return TS_E_PARAM; /* 未知键 = schema v1 之外（fail-closed） */
		}
	}
	uint8_t *wbuf = boot_wasm_buf_get();

	if (wbuf == NULL) {
		printk("[appmgr] boot[%u]: load buf alloc failed — no app loaded\n",
		       entry);
		return TS_E_NOMEM;
	}
	if (ts_store_slot_read(slot, TSAP_CONTENT_OFF + ml, wbuf, wl) != TS_OK) {
		return TS_E_IO;
	}
	/* 档位尺寸门（CG4 · LLD §5a 双门：small=16KB/large=256KB——按包 tier
	 * 取门；装载缓冲上限门维持在前）。schema 违例（small+needs 等）经
	 * scan_needs fail-closed。 */
	{
		ts_app_needs_t needs;

		if (ts_appmgr_scan_needs(man, ml, &needs) != TS_OK) {
			return TS_E_PARAM;
		}
		if (wl > ts_app_tier_gate(needs.tier)) {
			printk("[appmgr] boot[%u]: wasm %uB 超档位门 %uB (tier=%s)\n",
			       entry, wl, ts_app_tier_gate(needs.tier),
			       needs.tier == 1 ? "large" : "small");
			return TS_E_PARAM;
		}
#ifdef CONFIG_TS_HAL_FS
		/* CG5-B：资源白名单 boot 恢复（activate 绑定的重放——单一解析源
		 * scan_needs 的 res_csv；无 needs = 清除该 entry） */
		ts_appmgr_res_bind(entry + 1, needs.res_csv);
#endif
	}
	/* 数值 app_id = entry+1（M6 单实例装载 entry 0 → 1，与 V1 审计归因
	 * 兼容；M7 多实例 = 各 entry 各 id）。串身份随载注入（M9：kv 哈希源）。 */
	ts_appmgr_app_set_idstr(entry + 1, current_apps[entry].app_id);
	ts_res_t sr = ts_appmgr_app_start(entry + 1, wbuf, wl, boot_caps);

#ifdef CONFIG_TS_HAL_FS
	if (sr == TS_OK && boot_fs_paths[0] != '\0') {
		(void)ts_fs_paths_bind(entry + 1, boot_fs_paths); /* DEC-47⑤ */
	}
#endif
#ifdef CONFIG_TS_HAL_AV
	/* 完备掩码：图像 = fmt|w|h（0x7）；pcm16 = fmt|w（0x3，h 不存在） */
	{
		uint8_t need = (boot_av_fmt == TS_AV_FMT_PCM16) ? 0x3U : 0x7U;

		if (sr == TS_OK && boot_av_set == need) {
			(void)ts_av_config_bind(entry + 1, boot_av_fmt, boot_av_w,
						boot_av_h); /* DEC-47③ */
		}
	}
#endif
	return sr;
}
#endif /* CONFIG_TS_APP_WAMR */

ts_res_t ts_appmgr_boot_start(void)
{
	ts_appmgr_meta_t meta;

	if (ts_appmgr_meta_read(&meta) != TS_OK) {
		/* IR2-07：meta 不可读（空板/双损）= 不静默装载 slot 0 */
		printk("[appmgr] boot: meta unreadable — no app loaded\n");
		return TS_E_STATE;
	}
	ts_res_t first_res = TS_E_NOTFOUND; /* entry 0 结果（M6 单实例装载面） */

	for (uint8_t e = 0; e < meta.count; e++) {
		/* G4（单元 H）：隔离拒载（meta entry.rollback_count = LIMIT+1
		 * 为隔离持久化标记；计数 == LIMIT 仍可装载——与 runtime 语义
		 * 一致：允许恰好 3 次回滚）。观测面如实报 QUARANTINED。 */
		if (meta.entries[e].rollback_count > TS_APPMGR_ROLLBACK_LIMIT) {
			printk("[appmgr] boot[%u]: rollback_count=%u quarantined — no app loaded\n",
			       e, meta.entries[e].rollback_count);
			current_apps[e].active_slot = meta.entries[e].active_slot;
			current_apps[e].rollback_count = TS_APPMGR_ROLLBACK_LIMIT;
			current_apps[e].state = TS_APP_QUARANTINED;
			initialized = true;
			if (e == 0) {
				first_res = TS_E_ROLLBACK_LIMIT;
			}
			continue;
		}
#ifdef CONFIG_TS_APP_WAMR
		ts_res_t sr = boot_load_entry(e, &meta);

		if (e == 0) {
			first_res = sr; /* 透传（E_NOTFOUND/E_IO/E_INVALID_SIG/E_PARAM/sr） */
		}
		if (sr == TS_OK) {
			current_apps[e].state = TS_APP_ACTIVE; /* STAGED →（加载周期）→ ACTIVE */
			current_apps[e].active_slot = meta.entries[e].active_slot;
			/* B2 批修正：回滚计数随载恢复（跨重启 get-app 如实） */
			current_apps[e].rollback_count = meta.entries[e].rollback_count;
			/* 板级十修复：装载成功即占有——防 get_info 懒初始化打回
			 * STAGED（懒路径盲写 state = 装载结果被观测面抹掉）。 */
			initialized = true;
		}
		/* M7（Q-32③）：全部 entry 逐一装载（rt[] 并行面）——单 entry
		 * 失败不阻塞后续（合同 6：APP 故障不阻塞启动）。 */
#else
		return TS_E_STATE; /* WAMR 未编入（CONFIG_TS_APP_WAMR） */
#endif
	}
	return first_res;
}

#ifndef CONFIG_TS_APP_WAMR
/* 无 WAMR 构建面（netbench 等：TS_APPMGR=y 供 sys/app 命令链，但无运行
 * 时）。G4（单元 H）起 pkg.c 的 activate 路径引用本组符号——无 WAMR 面
 * 无构建覆盖，P4 无线批（netbench @ esp32p4）链接实证修复。语义退化：
 * 无运行时 = 恒无 APP 在跑；stop 无对象 = OK（无可停者）。 */
bool ts_appmgr_app_running(void)
{
	return false;
}

ts_res_t ts_appmgr_app_stop(void)
{
	return TS_OK;
}
#endif
