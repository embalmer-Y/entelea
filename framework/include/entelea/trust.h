/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_TRUST_H
#define ENTELEA_TRUST_H

/* R03 信任根表合同见 framework/services/storage/contract.md（§信任表）。
 * TUF root 同型全量替换表：撤销 = 次版剔除该键（无独立 revoked 列表——
 * 全量替换根除"定长撤销表溢出=旧键复活"面）。职责分层：本层只管持久化
 * 与 schema 确定性走查；签名/单调 ver 门控在 R12 编排链（上层）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_TRUST_KEYS_MAX 8
#define ENTELEA_TRUST_CBOR_MAX 512 /* schema 走查上限（Q-43 结构性） */

typedef struct {
	uint32_t ver;                        /* 单调版本（≥1；0=哨兵） */
	uint8_t n_keys;                      /* 0..MAX（0=全撤回→工厂根单源） */
	uint8_t keys[ENTELEA_TRUST_KEYS_MAX][32];
} entelea_trust_table_t;

/** [boot/首访] 惰性装载（活动副本 = seq 大且 CRC+schema 过者；双损/空 =
 * {0,0} 工厂根单源——fail-safe 侧）。幂等。 */
entelea_result_t entelea_trust_load(void);

/** [any] 只读指针（未装载则惰性装载）。 */
const entelea_trust_table_t *entelea_trust(void);

/** 确定性 schema 走查（map{"ver":uint≥1, "keys":[bstr(32)×0..8]}，恰
 * 2 对定序键；超上限/键内重复/域外/尾部余量 = E_IO）。 */
entelea_result_t entelea_trust_validate(const uint8_t *cbor, uint32_t len,
					entelea_trust_table_t *out);

/** [thread] 校验 → 乒乓写（写非活动副本→回读校验→活动切换→重装载）。
 * 非法表零写入（fail-closed）。调用方=R12 编排链（签名/ver 门控上层）。 */
entelea_result_t entelea_trust_write(const uint8_t *cbor, uint32_t len);

#ifdef ENTELEA_TEST
void entelea_trust_test_reset(void);
entelea_result_t entelea_trust_corrupt_test(uint8_t copy_idx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_TRUST_H */
