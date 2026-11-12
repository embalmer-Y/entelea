/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R12 试用状态机 + Forge 集成契约（新-only；合同见本目录 contract.md）。
 * 编程循环=OpenHands Software Agent SDK（owner 已确认）——SDK 侧循环
 * 不在固件内；本层=固件试用状态机（单候选/对象绑定确认/执行门/
 * fail-closed 超时与恢复）+ 工厂产物 intake（域外拒/未知 payload 拒）。
 * 工程通过≠用户验收：confirm 是唯一晋升路径（用户显式）。
 * 单线程假设：全部入口 [thread]，无内部锁（与 wasm 宿主同线程）。
 */
#include <string.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/storage.h>

static entelea_trial_state_t state;
static entelea_trial_candidate_t current;
static uint32_t last_epoch; /* 最近一次 submit 赋出的纪元（0=从未） */
static bool initialized;

static entelea_result_t ensure_initialized(void)
{
	return initialized ? ENTELEA_OK : entelea_trial_restore();
}

static void clear_candidate(void)
{
	memset(&current, 0, sizeof(current));
}

entelea_result_t entelea_trial_submit(const entelea_trial_candidate_t *cand)
{
	if (cand == NULL || cand->slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN) {
		return ENTELEA_E_STATE;
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
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot != ENTELEA_TRIAL_SLOT_UNKNOWN &&
	    cand->slot != current.slot) {
		return ENTELEA_E_BUSY; /* 槽保留：被拒/超时字节仍受门拒，
					 * 换槽会使其脱离试用门——重推须同槽 */
	}
	if (last_epoch == UINT32_MAX) {
		return ENTELEA_E_RANGE; /* 禁止回绕复用旧确认对象 */
	}
	current = *cand;
	last_epoch++;
	current.epoch = last_epoch;
	state = ENTELEA_TRIAL_PENDING_CONFIRM;
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_confirm(const entelea_trial_candidate_t *shown)
{
	if (shown == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	if (memcmp(shown->digest, current.digest, sizeof(current.digest)) != 0 ||
	    shown->permissions != current.permissions ||
	    shown->version != current.version || shown->slot != current.slot ||
	    shown->epoch != current.epoch) {
		return ENTELEA_E_PERM; /* 过期/不符的确认对象：保持 PENDING */
	}
	state = ENTELEA_TRIAL_ACTIVE; /* 用户显式确认——唯一晋升路径 */
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_reject(void)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_REJECTED; /* 槽保留（可重推） */
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_timeout(void)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_TIMEOUT; /* fail-closed 撤销（槽保留） */
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_end(void)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_STATE;
	}
	clear_candidate(); /* 纪元计数保留（跨候选单调） */
	state = ENTELEA_TRIAL_NONE;
	return ENTELEA_OK;
}

entelea_trial_state_t entelea_trial_state(void)
{
	return initialized ? state : ENTELEA_TRIAL_UNINITIALIZED;
}

void entelea_trial_current(entelea_trial_candidate_t *out)
{
	if (out != NULL) {
		if (initialized) {
			*out = current;
		} else {
			memset(out, 0, sizeof(*out));
			out->slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		}
	}
}

entelea_result_t entelea_trial_gate(uint8_t slot, const uint8_t digest[32])
{
	if (digest == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state == ENTELEA_TRIAL_NONE ||
	    (current.slot != ENTELEA_TRIAL_SLOT_UNKNOWN && slot != current.slot)) {
		return ENTELEA_OK; /* 试用政策不管辖（稳定集准入=D05） */
	}
	if (state == ENTELEA_TRIAL_ACTIVE &&
	    memcmp(digest, current.digest, sizeof(current.digest)) == 0) {
		return ENTELEA_OK; /* 仅用户已确认的那份字节 */
	}
	return ENTELEA_E_PERM;
}

/* ---- 工厂 intake（域外/未知 payload fail-closed） ---- */

entelea_result_t entelea_factory_intake(const entelea_factory_candidate_t *fc,
					uint8_t slot)
{
	if (fc == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
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
	tc.slot = slot;
	return entelea_trial_submit(&tc);
}

/* ---- 跨重启持久（板级批 A-4；RF1 v2）：记录布局（小端）
 * u8 ver=2 | u8 state | u8 cand_version | u8 slot | u16 permissions |
 * u32 last_epoch | digest[32]（42B）。v1 不迁移（按坏记录处置）。 */
#define TRIAL_PERSIST_VER 2U
#define TRIAL_REC_LEN (10U + 32U)

entelea_result_t entelea_trial_flush(void)
{
	uint8_t rec[TRIAL_REC_LEN];
	uint8_t verify[TRIAL_REC_LEN];
	uint16_t verify_len = 0;
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN) {
		return ENTELEA_E_STATE;
	}

	rec[0] = TRIAL_PERSIST_VER;
	rec[1] = (uint8_t)state;
	rec[2] = current.version;
	rec[3] = current.slot;
	rec[4] = (uint8_t)current.permissions;
	rec[5] = (uint8_t)(current.permissions >> 8);
	rec[6] = (uint8_t)last_epoch;
	rec[7] = (uint8_t)(last_epoch >> 8);
	rec[8] = (uint8_t)(last_epoch >> 16);
	rec[9] = (uint8_t)(last_epoch >> 24);
	memcpy(rec + 10, current.digest, sizeof(current.digest));
	r = entelea_storage_meta_write(rec, sizeof(rec));
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_meta_write(rec, sizeof(rec));
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_meta_read_mirrored(verify, sizeof(verify), &verify_len);
	if (r != ENTELEA_OK || verify_len != sizeof(rec) ||
	    memcmp(rec, verify, sizeof(rec)) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

static entelea_result_t meta_is_erased(bool *erased)
{
	uint8_t block[64];
	uint32_t size = entelea_storage_part_size(ENTELEA_PART_META);

	*erased = false;
	if (size != 2U * ENTELEA_STORAGE_META_REC) {
		return ENTELEA_E_IO;
	}
	for (uint32_t off = 0; off < size; off += sizeof(block)) {
		if (entelea_storage_read(ENTELEA_PART_META, off, block,
					 sizeof(block)) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		for (size_t i = 0; i < sizeof(block); i++) {
			if (block[i] != 0xFFU) {
				return ENTELEA_OK;
			}
		}
	}
	*erased = true;
	return ENTELEA_OK;
}

entelea_result_t entelea_trial_restore(void)
{
	uint8_t rec[TRIAL_REC_LEN];
	uint16_t len = 0;
	entelea_result_t r;

	if (initialized && !(state == ENTELEA_TRIAL_TIMEOUT &&
			     current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN)) {
		return ENTELEA_OK;
	}
	r = entelea_storage_meta_read_mirrored(rec, sizeof(rec), &len);

	clear_candidate();
	initialized = true;
	if (r == ENTELEA_E_IO) {
		bool erased = false;

		if (meta_is_erased(&erased) == ENTELEA_OK && erased) {
			state = ENTELEA_TRIAL_NONE;
			return ENTELEA_OK;
		}
		state = ENTELEA_TRIAL_TIMEOUT;
		current.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		return ENTELEA_E_IO;
	}
	const bool whole = r == ENTELEA_OK && len == sizeof(rec);
	const uint32_t ep = whole ? (uint32_t)rec[6] | ((uint32_t)rec[7] << 8) |
					    ((uint32_t)rec[8] << 16) |
					    ((uint32_t)rec[9] << 24)
				  : 0U;
	const bool valid = whole && rec[0] == TRIAL_PERSIST_VER &&
			   rec[1] <= ENTELEA_TRIAL_TIMEOUT &&
			   (rec[1] == ENTELEA_TRIAL_NONE ||
			    (rec[3] < ENTELEA_APP_SLOT_COUNT && ep != 0U));

	if (!valid) {
		/* 记录不自洽：候选槽不可知，两槽均受门拒。 */
		state = ENTELEA_TRIAL_TIMEOUT;
		current.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		return ENTELEA_E_IO;
	}
	if (ep > last_epoch) {
		last_epoch = ep; /* 单调：恢复不回退纪元计数 */
	}
	state = (entelea_trial_state_t)rec[1];
	if (state != ENTELEA_TRIAL_NONE) {
		current.version = rec[2];
		current.slot = rec[3];
		current.permissions = (uint16_t)(rec[4] | ((uint16_t)rec[5] << 8));
		current.epoch = ep;
		memcpy(current.digest, rec + 10, sizeof(current.digest));
	}
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_trial_test_reset(void)
{
	state = ENTELEA_TRIAL_NONE;
	clear_candidate();
	last_epoch = 0;
	initialized = false;
}
#endif
