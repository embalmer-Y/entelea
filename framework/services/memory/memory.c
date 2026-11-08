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
/* 墓碑列表（review P1 修复：单一墓碑被第二次 delete 覆盖会复活首个
 * 删除区间——改为累计列表）。容量有界；淘汰纪律（计划符合性审查 P2
 * 修复：原"满时覆盖 from 最小者"可使链内已删条目复活）：只复用被新区间
 * **完全覆盖**的槽（覆盖面不丢失）；无空槽且无被覆盖槽 = E_NOMEM
 * 显式拒（不复活任何已删区间）。全区间删除恒覆盖既有一切 → 永不因
 * 满而失败。 */
#define ENTELEA_MEM_TOMBSTONES_MAX 8
static struct {
	uint64_t from, to;
	bool active;
} tombstones[ENTELEA_MEM_TOMBSTONES_MAX];
static uint32_t tombstoned_count;

static bool is_tombstoned(uint64_t t)
{
	for (int i = 0; i < ENTELEA_MEM_TOMBSTONES_MAX; i++) {
		if (tombstones[i].active && t >= tombstones[i].from &&
		    t <= tombstones[i].to) {
			return true;
		}
	}
	return false;
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
	/* 有界且不复活：① 空槽；② 被新区间完全覆盖的槽（覆盖面不丢）；
	 * ③ 否则 E_NOMEM（显式拒——绝不覆盖仍有效的删除区间）。 */
	int slot = -1;
	int subsumed = -1;

	for (int i = 0; i < ENTELEA_MEM_TOMBSTONES_MAX; i++) {
		if (!tombstones[i].active) {
			slot = i;
			break;
		}
		if (subsumed < 0 && t_from_ms <= tombstones[i].from &&
		    tombstones[i].to <= t_to_ms) {
			subsumed = i; /* 保留首个被覆盖槽（循环继续找空槽） */
		}
	}
	if (slot < 0) {
		slot = subsumed;
	}
	if (slot < 0) {
		return ENTELEA_E_NOMEM; /* 满 + 无覆盖：拒（不复活） */
	}
	tombstones[slot].from = t_from_ms;
	tombstones[slot].to = t_to_ms;
	tombstones[slot].active = true;
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
	memset(tombstones, 0, sizeof(tombstones));
	tombstoned_count = 0;
}
#endif
