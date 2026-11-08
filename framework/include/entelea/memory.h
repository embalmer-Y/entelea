/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MEMORY_H
#define ENTELEA_MEMORY_H

/* R10 经历记忆合同见 framework/services/memory/contract.md。
 * 有界链+写入同意+来源+删除 tombstone；无同意不保存；删除后衍生
 * 索引不复活内容。恢复一致性：断电后链可恢复（RAM 后端=进程内）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/context.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_MEM_ENTRY_MAX 128   /* 单条载荷上限（建议值） */
#define ENTELEA_MEM_CHAIN_MAX 64    /* 链容量（有界——满=最旧让位+计数） */
#define ENTELEA_MEM_KIND_MAX 12

typedef struct {
	uint64_t t_ms;
	uint8_t source_kind;   /* entelea_source_kind_t */
	uint8_t kind;          /* 最低记忆类别（建议值枚举，消费方解释） */
	uint16_t len;
	uint8_t data[ENTELEA_MEM_ENTRY_MAX];
} entelea_mem_entry_t;

/** [thread] 写入（consent=false → E_PERM——采集≠记忆；满=最旧让位
 *  +evicted 计数——有界不拒写）。 */
entelea_result_t entelea_mem_write(bool consent,
				   const entelea_mem_entry_t *e);

/** [thread] 读最近 n 条（旧→新）；返回实际条数。 */
size_t entelea_mem_read(entelea_mem_entry_t *out, size_t max);

/** [thread] 删除（tombstone：删除后读取跳过该区间；不复活）。 */
entelea_result_t entelea_mem_delete(uint64_t t_from_ms, uint64_t t_to_ms);

/** [any] 链健康（条数+让位计数）。 */
typedef struct {
	uint16_t count;
	uint32_t evicted;
	uint32_t tombstoned;
} entelea_mem_health_t;

void entelea_mem_health(entelea_mem_health_t *out);

#ifdef ENTELEA_TEST
void entelea_mem_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_MEMORY_H */
