/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_APPSTORE_H
#define ENTELEA_APPSTORE_H

/* R05 包事实/验签/装载合同见 framework/services/package/contract.md。
 * 职责分层：本层管包事实（staging→verify→activate）与验签；执行闭环
 * 在 WAMR 宿主（wasm_host.c）；试用政策归 R12。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/package.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_APP_SLOT_COUNT 2
#define ENTELEA_APP_SLOT_SIZE 262144U
#define ENTELEA_TRUST_SET_MAX 10 /* 工厂 2 + 信任表 8 */

/* ---- staging→verify→activate 分步安装链（MA3.1 语义） ---- */

/** 开始暂存（擦 slot；slot ∈ [0, COUNT)）。 */
entelea_result_t entelea_pkg_stage_begin(uint8_t slot);

/** 追加块（顺序写 + 回读；越界/乱序拒）。 */
entelea_result_t entelea_pkg_stage_chunk(uint8_t slot, uint32_t off,
					 const void *buf, uint32_t len);

/** 全量验签：TSAP v2 头 + sha256 摘要对拍 + COSE_Sign1 ed25519
 * （tweetnacl）+ 内嵌副本比对（防签 A 装 B）。root=NULL = 有效信任集
 * （工厂根 ∪ R03 信任表）；失败 = E_INVALID_SIG（fail-closed，零激活）。
 * ENTELEA_TEST 构建验签对象 = 固定测试根（公开材料，测试信任≠生产）。 */
entelea_result_t entelea_pkg_verify(uint8_t slot, const uint8_t root_pubkey[32],
				    entelea_tsap_view_t *v, uint8_t digest_out[32]);

/** 激活（meta 记录 active slot；仅验签通过后可调）。 */
entelea_result_t entelea_pkg_activate(uint8_t slot);

/** 当前 active slot（无 = E_UNINIT）。 */
entelea_result_t entelea_pkg_active(uint8_t *slot);

#ifdef ENTELEA_TEST
/* 固定测试根（公开材料；生成脚本与私钥在旧 Forge fixtures）。 */
extern const uint8_t entelea_pkg_test_root_pub[32];
/* slot 直读（测试 oracle 面）。 */
entelea_result_t entelea_pkg_slot_read(uint8_t slot, uint32_t off, void *buf,
				       uint32_t len);
entelea_result_t entelea_pkg_slot_write(uint8_t slot, uint32_t off,
					const void *buf, uint32_t len);
entelea_result_t entelea_pkg_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_APPSTORE_H */
