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
 * （工厂根 ∪ R03 信任表）；格式/签名错误 = E_INVALID_SIG，存储读失败 =
 * E_IO；失败不激活。
 * ENTELEA_TEST 构建验签对象 = 固定测试根（公开材料，测试信任≠生产）。 */
entelea_result_t entelea_pkg_verify(uint8_t slot, const uint8_t root_pubkey[32],
				    entelea_tsap_view_t *v, uint8_t digest_out[32]);

/** 整文件摘要：sha256(slot[0, len))；len 取验签视图 pkg_len（=Forge
 * package_digest 输入域）。slot/len 域外=E_PARAM，读失败=E_IO。
 * 共享全局 sha256 流上下文——不可与其他流交错（单线程假设）。 */
entelea_result_t entelea_pkg_file_digest(uint8_t slot, uint32_t len,
					 uint8_t out[32]);

/** 单趟捕获：同一次流式读取计算 sha256(slot[0, len))，并把
 * slot[cap_off, cap_off+cap_len) 复制到 cap_buf（cap_len=0 时不捕获）。
 * 装载链用它保证门检查的摘要与实际装载字节来自同一批读取。
 * 域外（含区间越出 len）=E_PARAM，读失败=E_IO。 */
entelea_result_t entelea_pkg_file_digest_capture(uint8_t slot, uint32_t len,
						 uint8_t out[32], uint32_t cap_off,
						 uint32_t cap_len, uint8_t *cap_buf);

/** 激活（KV "PAGA" 记录 active slot）。它只是包**事实记录**，不授予
 * 执行权：装载链（wasm_host）装载前复核验签 + R12 试用门；稳定集
 * 准入/晋升见 delivery.h 的稳定记录（D-E）；激活本身不授予权限。 */
entelea_result_t entelea_pkg_activate(uint8_t slot);

/** 当前 active slot（无 = E_UNINIT；介质/初始化错误如实传播）。 */
entelea_result_t entelea_pkg_active(uint8_t *slot);

/** slot 直读（staged 包事实读面——装载链（wasm_host）取 wasm 段的
 * 生产路径；D-3 闸首次在非 TEST 构建编译 wasm_host 时发现其曾被误
 * 包在 TEST 块内）。 */
entelea_result_t entelea_pkg_slot_read(uint8_t slot, uint32_t off, void *buf,
				       uint32_t len);

#ifdef ENTELEA_TEST
/* 固定测试根（公开材料；生成脚本与私钥在旧 Forge fixtures）。 */
extern const uint8_t entelea_pkg_test_root_pub[32];
/* slot 直写（测试注入面——腐坏/越界 oracle；非生产路径）。 */
entelea_result_t entelea_pkg_slot_write(uint8_t slot, uint32_t off,
					const void *buf, uint32_t len);
entelea_result_t entelea_pkg_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_APPSTORE_H */
