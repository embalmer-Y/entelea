/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PROV_H
#define ENTELEA_PROV_H

/* R03 配置（provisioning）合同见 framework/services/config/contract.md。
 * 运行时只读（写通道仅烧录/维护窗）；装载面零写调用（机械检查目标）。
 * CBOR schema v1/v2（键序=v1 前缀+尾部追加，确定性子集解码）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ENTELEA_DEPLOY_ORCHESTRATED = 0, /* 断链→SAFE_LINKLOSS（缺省/缺字段） */
	ENTELEA_DEPLOY_STANDALONE = 1,   /* 独立运行：断链仅留痕不迁移 */
} entelea_deploy_mode_t;

typedef struct {
	char node_id[16];
	char cube_id[16];
	char router_locators[4][48];
	uint8_t root_pubkeys[2][32];
	char zenoh_cred[96];
	uint32_t power_budget_ma;
	uint8_t estop_trigger_flags;
	/* schema v2 尾部追加（缺字段缺省：orchestrated/远程清除开放/lconf 0） */
	uint8_t deploy_mode;        /* entelea_deploy_mode_t */
	uint8_t estop_remote_clear; /* 0/1 */
	uint8_t local_confirm;      /* 0/1：安装闸本地确认 */
	uint16_t tls_ca_len;
	uint16_t tls_cert_len;
	uint16_t tls_key_len;       /* 0 = TLS off；三键须同现同缺 */
} entelea_prov_t;

/* TLS DER 上限（P-256 实测 +~30% 余量，DEC-63③）。 */
#define ENTELEA_PROV_TLS_CA_MAX 768
#define ENTELEA_PROV_TLS_CERT_MAX 768
#define ENTELEA_PROV_TLS_KEY_MAX 256

typedef struct {
	uint8_t ca[ENTELEA_PROV_TLS_CA_MAX];
	uint8_t cert[ENTELEA_PROV_TLS_CERT_MAX];
	uint8_t key[ENTELEA_PROV_TLS_KEY_MAX];
	uint16_t ca_len;
	uint16_t cert_len;
	uint16_t key_len;
} entelea_prov_tls_t;

/** [boot/消费方] 装载（头 len|crc16 + CBOR；schema 走查失败/CRC 失败 =
 * ENTELEA_E_IO，调用方 fail-safe）。与 validate 同一解析路径。 */
entelea_result_t entelea_prov_load(void);

/** [any] 只读指针（未装载/失败兜底 = v1 等义安全侧；无运行时写接口）。 */
const entelea_prov_t *entelea_prov(void);

/** [any] TLS 三元组只读指针（len=0 = TLS off）。 */
const entelea_prov_tls_t *entelea_prov_tls(void);

/** [thread] 维护通道写入前校验（零拷贝走查，不触装载活态）。 */
entelea_result_t entelea_prov_validate(const uint8_t *cbor, uint32_t len);

/** [thread] 维护写：校验先行（失败零写入）+ per-boot 一次性窗（再写须
 * 复位循环——远程换身份结构性限速且可观测）。 */
entelea_result_t entelea_prov_write_maint(const uint8_t *cbor, uint32_t len);

#ifdef ENTELEA_TEST
/** 烧录通道测试注入（仅测试构建；生产烧录=外部工具）。 */
entelea_result_t entelea_prov_write_test(const uint8_t *cbor, uint32_t len);
/** 维护窗 RAM 旗复位（测试隔离=模拟复位循环）。 */
void entelea_prov_maint_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PROV_H */
