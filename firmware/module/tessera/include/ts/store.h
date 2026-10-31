/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ts-store 公共 API（design/LLD-ts-store.md）：分区抽象 / 掉电安全 meta /
 * 只读 provisioning / noinit 留痕 / APP slot 读写。
 * 合同关联：合同 10（prov 运行时只读）、DEC-05/23（slot 与 meta）。
 * 内部存储分区多字节整数 = **小端**（Zephyr 本机序；与 TSAP 容器的大端互不相关）。
 */
#ifndef TS_STORE_H__
#define TS_STORE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 分区模型（LLD-ts-store §2；DEC-23）-------------------------------- */

typedef enum {
	TS_PART_PROV,
	TS_PART_META,
	TS_PART_SLOT_A,
	TS_PART_SLOT_B,
	TS_PART_SLOT_C, /* V0.4.0 M6（Q-32②）：槽池扩至 4（每 APP 双槽乒乓 ×
			* TS_APP_MAX_INSTANCES=2）；TS_STORE_SLOT_COUNT<4 的构建
			* 不提供后端存储（size=0，slot_of 拒收） */
	TS_PART_SLOT_D,
	TS_PART_NOINIT,
	TS_PART_AUDIT, /* DEC-52①：审计 flash 环（8KB 两页乒乓；无此分区的板
			* 降级——audit init 留痕失败、内存环保留） */
	TS_PART_KV,    /* V0.4.0 M9（DR-15）：APP 业务状态 kv（8KB 两页乒乓；
			* 每 APP 有界 blob ≤250B，app_id 哈希归属——跨升级/回滚
			* 保留；CONFIG_TS_STORE_KV 门控） */
	TS_PART_TRUST, /* V0.8.0 EC2（Q-43③）：信任根表（8KB 双副本乒乓〔2×4KB 擦除块对齐〕——
			* 开发者公钥集 + 单调 ver；CONFIG_TS_STORE_TRUST 门控，
			* 缺分区 = 工厂根单源 = V0.7 行为逐字节等价） */
	TS_PART_COUNT,
} ts_store_part_t;

/* 初始化：后端就绪 + prov 加载（CRC 失败 → TS_E_IO，调用方进 fail-safe，合同 6）
 * + noinit 读取。boot 表接线随 M2b（periph/hal 步骤一次到位，保持数组纯追加）。 */
ts_res_t ts_store_init(void);

/* ---- 掉电安全 meta（§3）：双副本 + 序号 + CRC16-CCITT -------------------- */

ts_res_t ts_store_meta_write(const void *buf, uint16_t len);
ts_res_t ts_store_meta_read(void *buf, uint16_t *len);

/* ---- provisioning（§4）：CBOR schema v1/v2（DEC-30⑤ + CG2/Q-42），运行时只读 - */

/* 部署模式（CG2 · Q-41①/Q-42）：prov 声明，运行时不可切换（重配 = 重新
 * provision——显式可审计）。v1 prov 缺字段 = orchestrated（fail-safe 侧缺省）。 */
typedef enum {
	TS_DEPLOY_ORCHESTRATED = 0, /* 断链 → set_link(false) 全输出 SAFE_LINKLOSS（现行为） */
	TS_DEPLOY_STANDALONE = 1,   /* 独立运行：断链 = 留痕事件不迁移（LLD-ts-safety §3a） */
} ts_deploy_mode_t;

typedef struct {
	char node_id[16];
	char cube_id[16];
	char router_locators[4][48];
	uint8_t root_pubkeys[2][32];
	char zenoh_cred[96];
	uint32_t power_budget_ma;
	uint8_t estop_trigger_flags;
	/* —— schema v2（CG2 · Q-41/Q-42）：部署模式与 estop 远程清除通道 —— */
	uint8_t deploy_mode;        /* ts_deploy_mode_t；v1 缺字段 = orchestrated */
	uint8_t estop_remote_clear; /* 0=disabled/1=enabled；v1 缺字段 = enabled
					     * （现网兼容——无本地交互设备的唯一清除通道；
					     * LLD-ts-safety §5a） */
	/* —— schema v2 尾部追加第三键（F3 · DEC-62①）：安装闸本地确认 —— */
	uint8_t local_confirm;      /* 0=off（无头缺省）/1=on：activate 验签后置
					     * pending 待本地确认（60s fail-closed）；
					     * 11 对 v2 缺字段 = 0（存量 prov 零重烧） */
	/* —— schema v2 TLS 三元组（P1-2 · DEC-63③/Q-47③）：mTLS 供给（15 对）——
	 * DER 本体经 ts_store_prov_tls() 独立只读面（ts_prov_t 保持小足迹）；
	 * len=0 = 键缺省（12 对及以下 = TLS off，存量 prov 零重刷逐字节兼容）。 */
	uint16_t tls_ca_len;
	uint16_t tls_cert_len;
	uint16_t tls_key_len;
} ts_prov_t;

/* TLS 三元组上限出处：EC P-256 实测 DER（CA ~570B〔含 SAN〕/设备证书 ~590B/
 * SEC1 私钥 121B）+ ~30% 余量（DEC-63③）。 */
#define TS_PROV_TLS_CA_MAX 768
#define TS_PROV_TLS_CERT_MAX 768
#define TS_PROV_TLS_KEY_MAX 256

typedef struct {
	uint8_t ca[TS_PROV_TLS_CA_MAX];
	uint8_t cert[TS_PROV_TLS_CERT_MAX];
	uint8_t key[TS_PROV_TLS_KEY_MAX];
	uint16_t ca_len;
	uint16_t cert_len;
	uint16_t key_len;
} ts_prov_tls_t;

/* [any] TLS 三元组只读指针；ca_len=0 = TLS off（装载态镜像——validate
 * 维护通道走查不触碰活态）。消费者 = ts-net TLS glue（DEC-63③）；
 * 无 TLS 构建不提供（15 对 prov 仍装载 = TLS off）。 */
#ifdef CONFIG_TS_NET_TLS
const ts_prov_tls_t *ts_store_prov_tls(void);
#endif

ts_res_t ts_store_prov_load(void);      /* init 期一次；失败 = TS_E_IO */
const ts_prov_t *ts_store_prov(void);   /* [any] 只读指针；无运行时写接口（合同 10） */

/* V0.4.0 M10（Q-34）：维护通道（zenoh prov-write）——写入前全量 schema 校验
 * （与 load 同一确定性解析路径）+ per-boot 一次性窗（RAM 旗：每次远程换 prov
 * 必须经一次复位循环——可观测且限速；prov.c 本体零写不变，L5 纪律）。 */
ts_res_t ts_store_prov_validate(const uint8_t *cbor, uint32_t len);
ts_res_t ts_store_prov_write_maint(const uint8_t *cbor, uint32_t len);

/* ---- 信任根表（§4a；V0.8.0 EC2 · Q-43③/DEC-60；CONFIG_TS_STORE_TRUST 门控） */

#ifdef CONFIG_TS_STORE_TRUST
#define TS_TRUST_KEYS_MAX 8 /* 开发者键上限（Q-43① 常量门值） */

/* TUF root 同型全量替换表：撤销 = 次版剔除该键（无独立 revoked 列表——
 * 定长撤销表溢出 = 旧键复活面，全量替换根除；Q-43① 理由在册）。 */
typedef struct {
	uint32_t ver;                         /* 单调版本（严格 N+1 链） */
	uint8_t n_keys;                       /* 0..TS_TRUST_KEYS_MAX（0 = 全撤回工厂根单源） */
	uint8_t keys[TS_TRUST_KEYS_MAX][32];  /* ed25519 开发者公钥 */
} ts_trust_table_t;

/* 惰性装载（首访触发——只读无写回面；活动副本 = seq 大且 CRC 过者）。
 * 无记录/撕裂双损 = {ver=0, n_keys=0}（工厂根单源——fail-safe 侧）。 */
ts_res_t ts_store_trust_load(void);
const ts_trust_table_t *ts_store_trust(void); /* [any] 只读指针 */
/* 确定性 schema 走查（与 load 同一解析路径——单一事实源；prov.c 同型纪律）：
 * map{"ver":uint, "keys":[bstr(32)×0..8]}，2 对定序键；域外/不定长/
 * 键内重复/超上限 = TS_E_IO。 */
ts_res_t ts_store_trust_validate(const uint8_t *cbor, uint32_t len,
				 ts_trust_table_t *out);
/* 校验 → 乒乓写（写非活动副本 → 回读校验 → 成为新活动）→ RAM 表重载。
 * **唯一调用方 = ts-appmgr trust_update 编排链**（签名/ver 门控在上层）；
 * 本层只管持久化（prov_maint 同型职责分层）。 */
ts_res_t ts_store_trust_write(const uint8_t *cbor, uint32_t len);
#endif

/* ---- noinit 留痕（§5）：复位原因 / WDT 报告 ------------------------------ */

void ts_store_noinit_put(const void *rec, uint16_t len);
ts_res_t ts_store_noinit_get(void *rec, uint16_t *len, bool *fresh);

/* ---- APP slot 读写（§6；逐块写 + 回读校验）------------------------------- */

ts_res_t ts_store_slot_write(uint8_t slot, uint32_t off, const void *buf, uint32_t len);
ts_res_t ts_store_slot_read(uint8_t slot, uint32_t off, void *buf, uint32_t len);
ts_res_t ts_store_slot_hash(uint8_t slot, uint8_t sha[32]);
/* 全 slot 抹除：安装序前置（flash 后端必须——物理只可 1→0；RAM 后端 = 0xFF
 * 复位，语义一致）。消费者 = ts-appmgr stage_begin（板级五新增，LLD §6 增补）。 */
ts_res_t ts_store_slot_erase(uint8_t slot);

/* ---- 审计 flash 环（DEC-52①；掉电留存安全事件）--------------------------- */

#ifdef CONFIG_TS_STORE_AUDIT
#include <ts/safety.h>

/* flash 槽解码形态（64B 定长槽的小端字段视图；evt 扩展字段在 store 域） */
typedef struct {
	uint64_t t_ms;
	uint32_t seq;
	uint16_t ch_idx; /* 0xFFFF = 系统事件（estop 等） */
	uint16_t actor;
	uint8_t kind;
	int32_t res;   /* ts_res_t（负值错误码——u8 会截断，V2-2 实测拦下） */
	uint16_t evt; /* 0 = commit 审计；1 = estop 沿触发 */
	uint32_t value_u;
} ts_store_audit_entry_t;

ts_res_t ts_store_audit_init(void);   /* 扫描两页恢复游标；分区缺失 = 留痕降级 */
/* 安全事件落盘（commit 异常〔res != TS_OK〕+ estop 沿）；写满翻页（擦对页） */
ts_res_t ts_store_audit_append(const ts_audit_entry_t *e, uint16_t evt);
/* 按 seq 升序读最近 max 条（跨两页过渡态 ≤128）；返回条数 */
size_t ts_store_audit_read(ts_store_audit_entry_t *out, size_t max);
void ts_store_audit_boot_report(void); /* boot 打印最近 8 条（人读留痕） */
#endif

/* ---- APP 业务状态 kv（V0.4.0 M9，DR-15；CONFIG_TS_STORE_KV 门控）---------- */
#ifdef CONFIG_TS_STORE_KV
#define TS_KV_BLOB_MAX 250 /* 槽 264B − 14B 头（结构性数值） */

/** 惰性初始化（首次 save/load 内部调用亦可）；分区缺失 = E_STATE 如实。 */
ts_res_t ts_store_kv_init(void);
/** 保存/覆盖该 app 的状态 blob（app_hash = app_id 串哈希——跨升级/回滚
 *  归属不变，DR-15）。len ≤ TS_KV_BLOB_MAX；页满自动压实（两页乒乓）。 */
ts_res_t ts_store_kv_save(uint32_t app_hash, const uint8_t *data, uint16_t len);
/** 读该 app 最新 blob；返回拷贝长度（≥0）或 ts_res_t（负值：未存 = E_NOTFOUND，
 *  容量不足 = E_RANGE）。 */
int32_t ts_store_kv_load(uint32_t app_hash, uint8_t *out, uint16_t max);
#endif

/* ---- 测试钩子（仅 CONFIG_TS_TEST）---------------------------------------- */
#ifdef CONFIG_TS_TEST
/* 抹除全部后端存储（测试隔离：每用例套件 setup 调用） */
ts_res_t ts_store_test_reset(void);
/* prov 烧录通道注入（生产烧录 = 外部工具/Agent deploy_push_prov，MA3；合同 10：
 * prov.c 本体零写调用——本函数在 prov_test.c，L5 检查目标即 prov.c）。 */
ts_res_t ts_store_prov_write_test(const uint8_t *cbor, uint32_t len);
/* 主动破坏一份 meta 副本（撕裂注入） */
ts_res_t ts_store_meta_corrupt_test(uint8_t copy_idx);
#ifdef CONFIG_TS_STORE_KV
/* kv 游标复位（后端抹除后重扫描；V0.4.0 M9 测试隔离） */
void ts_store_kv_test_reset(void);
#endif
#ifdef CONFIG_TS_STORE
/* prov 维护窗 RAM 旗复位（V0.4.0 M10 测试隔离） */
void ts_store_prov_maint_test_reset(void);
#endif
#ifdef CONFIG_TS_STORE_TRUST
/* 信任表内存态复位（后端抹除后重装载；测试隔离） */
void ts_store_trust_test_reset(void);
/* 主动破坏一份信任表副本（撕裂注入——meta_corrupt 同型） */
ts_res_t ts_store_trust_corrupt_test(uint8_t copy_idx);
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* TS_STORE_H__ */
