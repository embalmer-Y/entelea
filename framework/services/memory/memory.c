/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R10 经历记忆（新-only N-MEMORY；合同见本目录 contract.md）。
 * 有界链（满=最旧让位+计数）；写入须同意（consent 显式——采集≠记忆）；
 * 删除=tombstone 区间（读取跳过；衍生面不复活）；恢复一致性=链序
 * 单调（RAM 后端进程内——持久后端随板级批）。
 */
#include <string.h>
#include <entelea/memory.h>

static entelea_mem_entry_t chain[ENTELEA_MEM_CHAIN_MAX];
static uint32_t chain_len;
static uint32_t evicted_count;
static struct {
	uint64_t from, to;
	bool active;
} tombstone;
static uint32_t tombstoned_count;

static bool is_tombstoned(uint64_t t)
{
	return tombstone.active && t >= tombstone.from && t <= tombstone.to;
}

entelea_result_t entelea_mem_write(bool consent, const entelea_mem_entry_t *e)
{
	if (e == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (!consent) {
		return ENTELEA_E_PERM; /* 无同意不保存（采集≠记忆） */
	}
	if (e->len == 0 || e->len > ENTELEA_MEM_ENTRY_MAX) {
		return ENTELEA_E_PARAM;
	}
	if (chain_len == ENTELEA_MEM_CHAIN_MAX) {
		memmove(&chain[0], &chain[1],
			(chain_len - 1) * sizeof(chain[0]));
		chain_len--;
		evicted_count++; /* 有界：最旧让位（不拒写） */
	}
	chain[chain_len++] = *e;
	return ENTELEA_OK;
}

size_t entelea_mem_read(entelea_mem_entry_t *out, size_t max)
{
	if (out == NULL) {
		return 0;
	}
	size_t n = 0;

	for (uint32_t i = 0; i < chain_len && n < max; i++) {
		if (is_tombstoned(chain[i].t_ms)) {
			continue; /* 删除后不复活 */
		}
		out[n++] = chain[i];
	}
	return n;
}

entelea_result_t entelea_mem_delete(uint64_t t_from_ms, uint64_t t_to_ms)
{
	if (t_from_ms > t_to_ms) {
		return ENTELEA_E_PARAM;
	}
	tombstone.from = t_from_ms;
	tombstone.to = t_to_ms;
	tombstone.active = true;
	tombstoned_count++;
	return ENTELEA_OK;
}

void entelea_mem_health(entelea_mem_health_t *out)
{
	if (out == NULL) {
		return;
	}
	out->count = (uint16_t)chain_len;
	out->evicted = evicted_count;
	out->tombstoned = tombstoned_count;
}

#ifdef ENTELEA_TEST
void entelea_mem_test_reset(void)
{
	memset(chain, 0, sizeof(chain));
	chain_len = 0;
	evicted_count = 0;
	memset(&tombstone, 0, sizeof(tombstone));
	tombstoned_count = 0;
}
#endif
