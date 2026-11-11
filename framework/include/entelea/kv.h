/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_KV_H
#define ENTELEA_KV_H

/* R03 App 业务状态 kv 合同见 framework/services/storage/contract.md。
 * 有界 blob（≤250B）按 app_id 哈希归属，跨升级/回滚保留；不是经历库。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_KV_BLOB_MAX 250 /* 264B 槽 − 14B 头（结构性） */

/** 惰性初始化（首次 save/load 内部调用亦可）；分区缺失 = E_STATE，
 *  读错误 = E_IO 且不就绪（下次调用重扫，不盲写）。 */
entelea_result_t entelea_kv_init(void);

/** 保存/覆盖该 app 的状态 blob（追加式；体先写、提交标记后写；页满
 *  先读后擦压实）。失败 = 错误码（init 错误原样；写失败 E_IO 且该槽
 *  烧毁）——失败不改变已存的最新值。 */
entelea_result_t entelea_kv_save(uint32_t app_hash, const uint8_t *data,
				 uint16_t len);

/** 读该 app 最新 blob；返回拷贝长度（≥0）或错误码（out 空 = E_PARAM，
 *  未存 = E_NOTFOUND，容量不足 = E_RANGE，读错误 = E_IO，init 错误
 *  原样）。读错误≠未存。 */
int32_t entelea_kv_load(uint32_t app_hash, uint8_t *out, uint16_t max);

#ifdef ENTELEA_TEST
/** 游标复位（后端抹除后重扫描；测试隔离）。 */
void entelea_kv_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_KV_H */
