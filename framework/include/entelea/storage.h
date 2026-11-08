/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_STORAGE_H
#define ENTELEA_STORAGE_H

/* R03 存储端口合同见 framework/services/storage/contract.md。
 * 独立 namespace：每个分区是隔离命名空间，跨区越界拒绝；后端不可知。
 * 内部分区多字节整数 = 小端（与 TSAP 容器大端互不相关）。
 * 相对旧 ts-store 的裁剪与延后子集记录于合同与台账。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 分区命名空间（编号只追加不改既有值；未实现面按子闭环逐批加入）。 */
typedef enum {
	ENTELEA_PART_META,   /* 掉电安全小状态（双副本） */
	ENTELEA_PART_NOINIT, /* 复位留痕（一次性） */
	ENTELEA_PART_COUNT,
} entelea_store_part_t;

/** [boot] 存储初始化（后端就绪 + noinit 消费）。失败显式返回；
 * 上层按 fail-safe 处置。 */
entelea_result_t entelea_storage_init(void);

/** [boot] 上次异常复位留痕（init 消费后可查；present=false = 无/损坏）。 */
void entelea_storage_last_reset(uint32_t *reason, bool *present);

/* ---- 掉电安全 meta：双副本 + 单调序号 + CRC16-CCITT ---- */

/** [thread] 写非活动副本 → 回读校验 → 成为新活动（seq 单调递增；
 * 撕裂时旧副本仍完整）。len ≤ ENTELEA_STORAGE_META_MAX。 */
entelea_result_t entelea_storage_meta_write(const void *buf, uint16_t len);

/** [thread] 活动 = 序号较大且 CRC 通过者；单副本损毁可恢复；
 * 双副本皆损 = ENTELEA_E_IO（调用方按缺省安全态/fail-safe，不伪造）。 */
entelea_result_t entelea_storage_meta_read(void *buf, uint16_t *len);

/* ---- noinit 留痕（一次性） ---- */

/** [any] 留痕（覆盖式；失败不可返回——静默丢弃，读取面如实呈现无留痕）。 */
void entelea_storage_noinit_put(const void *rec, uint16_t len);

/** [thread] 读留痕：fresh=true = 无/损坏（按无留痕处理）；读到有效记录
 * = fresh=false 且**读后抹除**（one-shot：防陈旧留痕跨复位误报）。 */
entelea_result_t entelea_storage_noinit_get(void *rec, uint16_t *len, bool *fresh);

/* 尺寸出处 DEC-23/27 语义（基线结构性数值；板级按 DT 调整）。 */
#define ENTELEA_STORAGE_META_MAX 256
#define ENTELEA_STORAGE_NOINIT_MAX_REC 250

/* 仅 ENTELEA_TEST 构建：抹除全部分区（测试隔离）；撕裂注入。 */
#ifdef ENTELEA_TEST
entelea_result_t entelea_storage_test_reset(void);
entelea_result_t entelea_storage_meta_corrupt_test(uint8_t copy_idx);
/* 端口边界负测探针：直通后端 read。 */
entelea_result_t entelea_storage_test_backend_read(entelea_store_part_t part,
						   uint32_t off, void *buf,
						   uint32_t len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_STORAGE_H */
