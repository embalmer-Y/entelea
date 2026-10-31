/* SPDX-License-Identifier: Apache-2.0 */
/* ts-appmgr 内部共享（不对外）。 */
#ifndef TS_APPMGR_INTERNAL_H__
#define TS_APPMGR_INTERNAL_H__

#include <ts/appmgr.h>
#include <ts/tsap.h>
#include <stdbool.h>

/* 运行时 APP 状态（slot.c 定义，pkg.c 引用）——M6 起按 entry 索引
 * （current_apps[e]；legacy 单 APP 语义 = entry 0） */
extern ts_app_info_t current_apps[CONFIG_TS_APP_MAX_INSTANCES];
extern bool initialized;

/* F3 安装闸挂起态（pkg.c 定义，slot.c get_info 消费——DEC-62①）：仅内存
 *（复位即失 = fail-closed）；确认/拒绝仅固件内本地路。 */
struct ts_appmgr_pend {
	bool active;
	uint8_t slot;
	uint8_t entry;
};
extern struct ts_appmgr_pend pending;
#ifdef CONFIG_TS_TEST
void ts_appmgr_pend_reset_test(void); /* 测试隔离：pending+stage 清台 */
#endif

/* P1-1（HLD §3.4）：rt 串身份查找（消息路由/测试面）+ ctx 取得
 *（natives.c 同源——防伪造注入值）。 */
struct ts_app_rt;
ts_ctx_t ts_app_rt_ctx(struct ts_app_rt *rt);
struct ts_app_rt *ts_appmgr_rt_by_str(const char *app_id_str);

/* verify.c（DEC-49）：全量验签（头 v2 → 摘要 → COSE ed25519）与 boot 快校验 */
ts_res_t ts_appmgr_verify_slot(uint8_t slot, const uint8_t root_pubkey[32],
			       tsap_view_t *v, uint8_t digest_out[32]);
ts_res_t ts_appmgr_check_digest(uint8_t slot, tsap_view_t *v);
const uint8_t *ts_appmgr_root_key(const uint8_t root_pubkey[32]);

/* 有效信任集（EC2 · Q-43②）：工厂根（TEST=测试根/生产=pk0+pk1）∪ 信任表
 * keys（去重）；keys_out 容量 ≥ 2+TS_TRUST_KEYS_MAX。返回键数（恒 ≥1）。 */
#define TS_APPMGR_TRUST_SET_MAX (2 + 8)
uint8_t ts_appmgr_trust_effective(const uint8_t *keys_out[TS_APPMGR_TRUST_SET_MAX]);

/* ---- 资源档位（CG4 · Q-39②/Q-42 · LLD-ts-appmgr §5a）--------------------- */
#ifdef CONFIG_TS_APP_WAMR

/* manifest v2 tier/needs 扫描结果（ts_appmgr_scan_needs 填充） */
typedef struct {
	uint8_t tier;            /* 0=small（缺省）/1=large */
	bool has_needs;          /* needs 键出现（仅 large 合法） */
	uint32_t linear_memory_kb; /* 声明线性内存预算（0 = 未声明） */
	uint32_t stack_kb;         /* 声明栈预算（0 = 未声明；V1 记录不消费） */
	uint16_t resources;        /* 资源路径条数（fs 前缀需求面） */
#ifdef CONFIG_TS_HAL_FS
	char res_csv[CONFIG_TS_HAL_FS_PATHS_MAX]; /* CG5-B：资源前缀 CSV（';' 分隔
		 *——activate/boot 绑定 res registry 的数据源；容量对齐 fs_paths） */
#endif
} ts_app_needs_t;

/** manifest 键分发扫描：跳过非 tier/needs 键（ts_cbor_skip），仅提取档位
 * 声明。schema 纪律：tier ∈ {"small","large"}；needs 仅 large 可携
 *（small+needs = TS_E_PARAM）；needs 为 map{linear_memory_kb?, stack_kb?,
 * resources[]?}——resources 条目 = 绝对路径无 ".."（fs_paths 同文法）。
 * 任何结构/域值违例 = TS_E_PARAM（fail-closed，与 boot 走查同纪律）。 */
ts_res_t ts_appmgr_scan_needs(const uint8_t *man, uint32_t ml, ts_app_needs_t *out);

/** 安装期匹配矩阵（LLD §5a；stage_verify 验签后调用）：
 * ① needs.linear_memory_kb ≤ 设备线性内存帽（CONFIG_TS_APP_WAMR_HEAP 推导）
 * ② needs.resources>0 须 CONFIG_TS_HAL_FS
 * ③ tier=large 须设备支持（TS_APP_LOAD_MAX ≥ TS_APP_SIZE_GATE_LARGE）
 * 任一不满足 = TS_E_RANGE（安装期拒装替代运行期 OOM——Android
 * getMemoryClass 同型）。返回 TS_OK = 匹配通过。 */
ts_res_t ts_appmgr_needs_match(const ts_app_needs_t *n);

/** 档位取尺寸门（LLD §5a 双门；boot 装载与 Agent 编译链同值镜像） */
static inline uint32_t ts_app_tier_gate(uint8_t tier)
{
	return tier == 1 ? (uint32_t)CONFIG_TS_APP_SIZE_GATE_LARGE
			 : (uint32_t)CONFIG_TS_APP_SIZE_GATE_SMALL;
}

#endif /* CONFIG_TS_APP_WAMR */

#endif /* TS_APPMGR_INTERNAL_H__ */
