/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R12 试用状态机 + Forge 集成契约（新-only；合同见本目录 contract.md）。
 * 编程循环=OpenHands Software Agent SDK（owner 已确认）——SDK 侧循环
 * 不在固件内；本层=固件试用状态机（单候选/内容绑定确认/fail-closed
 * 超时）+ 工厂产物 intake（域外拒/未知 payload 拒）。
 * 工程通过≠用户验收：confirm 是唯一晋升路径（用户显式）。
 */
#include <string.h>
#include <entelea/delivery.h>
#include <entelea/storage.h>

static entelea_trial_state_t state;
static entelea_trial_candidate_t current;

entelea_result_t entelea_trial_submit(const entelea_trial_candidate_t *cand)
{
	if (cand == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (state == ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_BUSY; /* 单候选：ACTIVE 期不受理新候选 */
	}
	if (state == ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_BUSY; /* review P1 修复：PENDING 期不受理
					 * 新候选——已展示的确认对象不可被
					 * 静默替换（内容绑定完整性）。
					 * reject/timeout 先行释放。 */
	}
	current = *cand;
	state = ENTELEA_TRIAL_PENDING_CONFIRM;
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_confirm(void)
{
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_ACTIVE; /* 用户显式确认——唯一晋升路径 */
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_reject(void)
{
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_REJECTED; /* 槽保留（可重推） */
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_timeout(void)
{
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_TIMEOUT; /* fail-closed 撤销（槽保留） */
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_end(void)
{
	if (state != ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_STATE;
	}
	memset(&current, 0, sizeof(current));
	state = ENTELEA_TRIAL_NONE;
	return ENTELEA_OK;
}

entelea_trial_state_t entelea_trial_state(void)
{
	return state;
}

void entelea_trial_current(entelea_trial_candidate_t *out)
{
	if (out != NULL) {
		*out = current;
	}
}

/* ---- 工厂 intake（域外/未知 payload fail-closed） ---- */

entelea_result_t entelea_factory_intake(const entelea_factory_candidate_t *fc)
{
	if (fc == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (fc->payload_kind > 1) {
		return ENTELEA_E_PARAM; /* 不默认解释为 wasm（后缀不推断权限） */
	}
	if (fc->budget_steps == 0 || fc->budget_steps > 10000000U ||
	    fc->mem_quota_kb == 0 || fc->mem_quota_kb > 512) {
		return ENTELEA_E_RANGE; /* 预算域外拒（R05 Lua 合同同域） */
	}
	entelea_trial_candidate_t tc;

	memset(&tc, 0, sizeof(tc));
	memcpy(tc.digest, fc->package_digest, 32);
	tc.permissions = fc->permissions;
	tc.version = fc->runtime_version;
	return entelea_trial_submit(&tc);
}

/* ---- 跨重启持久（板级批 A-4）：记录布局（小端）
 * u8 ver=1 | u8 state | u8 cand_version | u8 rsv | u16 permissions |
 * digest[32]。恢复 fail-closed：任意不符 → NONE。 */
#define TRIAL_PERSIST_VER 1U

entelea_result_t entelea_trial_flush(void)
{
	uint8_t rec[4 + 2 + 32];

	rec[0] = TRIAL_PERSIST_VER;
	rec[1] = (uint8_t)state;
	rec[2] = current.version;
	rec[3] = 0;
	rec[4] = (uint8_t)current.permissions;
	rec[5] = (uint8_t)(current.permissions >> 8);
	memcpy(rec + 6, current.digest, sizeof(current.digest));
	return entelea_storage_meta_write(rec, sizeof(rec));
}

entelea_result_t entelea_trial_restore(void)
{
	uint8_t rec[4 + 2 + 32];
	uint16_t len = 0;

	if (entelea_storage_meta_read(rec, &len) != ENTELEA_OK ||
	    len != sizeof(rec) || rec[0] != TRIAL_PERSIST_VER ||
	    rec[1] > ENTELEA_TRIAL_TIMEOUT) {
		/* 无记录/坏记录：fail-closed 归 NONE（不复活垃圾）。 */
		state = ENTELEA_TRIAL_NONE;
		memset(&current, 0, sizeof(current));
		return ENTELEA_OK;
	}
	state = (entelea_trial_state_t)rec[1];
	current.version = rec[2];
	current.permissions = (uint16_t)(rec[4] | ((uint16_t)rec[5] << 8));
	memcpy(current.digest, rec + 6, sizeof(current.digest));
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_trial_test_reset(void)
{
	state = ENTELEA_TRIAL_NONE;
	memset(&current, 0, sizeof(current));
}
#endif
