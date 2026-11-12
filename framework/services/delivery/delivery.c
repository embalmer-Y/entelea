/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R12 试用状态机 + Forge 集成契约（新-only；合同见本目录 contract.md）。
 * 编程循环=OpenHands Software Agent SDK（owner 已确认）——SDK 侧循环
 * 不在固件内；本层=固件试用状态机（单候选/对象绑定确认/执行门/
 * fail-closed 超时与恢复）+ 工厂产物 intake（域外拒/未知 payload 拒）。
 * 工程通过≠用户验收：confirm 开启试用；真实用户授权由可信调用方负责。
 * 单线程假设：全部入口 [thread]，无内部锁（与 wasm 宿主同线程）。
 */
#include <string.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/principal.h>
#include <entelea/storage.h>

static entelea_trial_state_t state;
static entelea_trial_candidate_t current;
static uint32_t last_epoch; /* 最近一次 submit 赋出的纪元（0=从未） */
static bool initialized;

static entelea_result_t
ensure_initialized(void)
{
	return initialized ? ENTELEA_OK : entelea_trial_restore();
}

static void
clear_candidate(void)
{
	memset(&current, 0, sizeof(current));
}

/* 状态检查（不改变状态）：拒绝顺序与旧 submit 一致。 */
static entelea_result_t
admit_check(uint8_t slot)
{
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
		return ENTELEA_E_BUSY; /* PENDING 期不受理：已展示的确认对象不可被替换 */
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot != ENTELEA_TRIAL_SLOT_UNKNOWN &&
	    slot != current.slot) {
		return ENTELEA_E_BUSY; /* 槽保留：被拒/超时字节仍受门拒，重推须同槽 */
	}
	if (last_epoch == UINT32_MAX) {
		return ENTELEA_E_RANGE; /* 禁止回绕复用旧确认对象 */
	}
	return ENTELEA_OK;
}

static bool
perm_valid(const char *perm, uint8_t len)
{
	entelea_perm_table_t t;

	if (len > ENTELEA_TRIAL_PERM_MAX) {
		return false;
	}
	entelea_perm_table_init(&t);
	return entelea_perm_parse_list(perm, len, &t) == ENTELEA_OK;
}

static bool
budget_valid(uint32_t steps)
{
	return steps >= 1U && steps <= ENTELEA_TRIAL_STEPS_MAX;
}

static void
admit_commit(const entelea_trial_candidate_t *c)
{
	current = *c;
	last_epoch++;
	current.epoch = last_epoch;
	state = ENTELEA_TRIAL_PENDING_CONFIRM;
}

/* 设备计算候选身份：验签后的整文件摘要；expect 非空时必须与之相等。 */
static entelea_result_t
admit_from_slot(const entelea_trial_request_t *req, const uint8_t *expect)
{
	entelea_tsap_view_t v;
	entelea_trial_candidate_t c;
	entelea_result_t r;

	if (req == NULL || req->slot >= ENTELEA_APP_SLOT_COUNT ||
	    !perm_valid(req->perm, req->perm_len)) {
		return ENTELEA_E_PARAM;
	}
	if (!budget_valid(req->budget_steps)) {
		return ENTELEA_E_RANGE;
	}
	r = admit_check(req->slot);
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_pkg_verify(req->slot, NULL, &v, NULL);
	if (r != ENTELEA_OK) {
		return r; /* 槽内无有效包：不改变状态 */
	}
	memset(&c, 0, sizeof(c));
	r = entelea_pkg_file_digest(req->slot, v.pkg_len, c.digest);
	if (r != ENTELEA_OK) {
		return r;
	}
	if (expect != NULL && memcmp(c.digest, expect, sizeof(c.digest)) != 0) {
		return ENTELEA_E_INVALID_SIG; /* 包内容与工厂声明的摘要不符 */
	}
	memcpy(c.perm, req->perm, req->perm_len);
	c.perm_len = req->perm_len;
	c.budget_steps = req->budget_steps;
	c.version = req->version;
	c.slot = req->slot;
	admit_commit(&c);
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_submit(const entelea_trial_request_t *req)
{
	return admit_from_slot(req, NULL);
}

/* 展示对象与当前候选逐字段相等（确认与晋升共用）。 */
static bool
candidate_matches(const entelea_trial_candidate_t *shown)
{
	return memcmp(shown->digest, current.digest, sizeof(current.digest)) ==
		       0 &&
	       shown->perm_len == current.perm_len &&
	       memcmp(shown->perm, current.perm, current.perm_len) == 0 &&
	       shown->budget_steps == current.budget_steps &&
	       shown->version == current.version &&
	       shown->slot == current.slot && shown->epoch == current.epoch;
}

entelea_result_t
entelea_trial_confirm(const entelea_trial_candidate_t *shown)
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
	if (!candidate_matches(shown)) {
		return ENTELEA_E_PERM; /* 过期/不符的确认对象：保持 PENDING */
	}
	state = ENTELEA_TRIAL_ACTIVE; /* 用户显式确认——唯一晋升路径 */
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_reject(void)
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

entelea_result_t
entelea_trial_timeout(void)
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

entelea_result_t
entelea_trial_end(void)
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

entelea_trial_state_t
entelea_trial_state(void)
{
	return initialized ? state : ENTELEA_TRIAL_UNINITIALIZED;
}

void
entelea_trial_current(entelea_trial_candidate_t *out)
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

entelea_result_t
entelea_trial_gate(uint8_t slot, const uint8_t digest[32])
{
	if (digest == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state == ENTELEA_TRIAL_NONE ||
	    (current.slot != ENTELEA_TRIAL_SLOT_UNKNOWN &&
	     slot != current.slot)) {
		return ENTELEA_OK; /* 试用政策不管辖（稳定集准入=D05） */
	}
	if (state == ENTELEA_TRIAL_ACTIVE &&
	    memcmp(digest, current.digest, sizeof(current.digest)) == 0) {
		return ENTELEA_OK; /* 仅用户已确认的那份字节 */
	}
	return ENTELEA_E_PERM;
}

/* 授权查询：仅 ACTIVE 且槽与摘要均相符时授予确认的权限与步数。 */
entelea_result_t
entelea_trial_grant(uint8_t slot, const uint8_t digest[32],
		    entelea_trial_grant_t *out)
{
	if (digest == NULL || out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_ACTIVE || current.slot != slot ||
	    memcmp(digest, current.digest, sizeof(current.digest)) != 0) {
		return ENTELEA_E_PERM;
	}
	memcpy(out->perm, current.perm, current.perm_len);
	out->perm_len = current.perm_len;
	out->budget_steps = current.budget_steps;
	return ENTELEA_OK;
}

/* ---- 工厂 intake（域外/未知 payload fail-closed） ---- */

entelea_result_t
entelea_factory_intake(const entelea_factory_candidate_t *fc, uint8_t slot)
{
	if (fc == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (fc->payload_kind > 1) {
		return ENTELEA_E_PARAM; /* 不默认解释为 wasm（后缀不推断权限） */
	}
	if (fc->budget_steps == 0 || fc->budget_steps > 10000000U ||
	    fc->mem_quota_kb == 0 || fc->mem_quota_kb > 512 ||
	    fc->permissions_len > ENTELEA_TRIAL_PERM_MAX) {
		return ENTELEA_E_RANGE; /* 预算/权限长度域外拒（R05 Lua 合同同域） */
	}
	entelea_trial_request_t req;

	memset(&req, 0, sizeof(req));
	req.slot = slot;
	req.version = fc->runtime_version;
	req.budget_steps = fc->budget_steps;
	memcpy(req.perm, fc->permissions, fc->permissions_len);
	req.perm_len = fc->permissions_len;
	return admit_from_slot(&req, fc->package_digest);
}

/* ---- 跨重启持久（板级批 A-4；RF1 v3）：记录布局（小端）
 * u8 ver=3 | u8 state | u8 cand_version | u8 slot | u32 last_epoch |
 * u32 budget_steps | u8 perm_len | perm[96]（零填充）| digest[32]。
 * v1/v2 不迁移：按坏记录处置（fail-closed）。 */
#define TRIAL_PERSIST_VER 4U
#define TRIAL_REC_PERM_OFF 13U
#define TRIAL_REC_DIG_OFF (TRIAL_REC_PERM_OFF + ENTELEA_TRIAL_PERM_MAX)
#define TRIAL_REC_LEN (TRIAL_REC_DIG_OFF + 32U)
/* 同一双副本 meta 记录内追加每槽稳定项：present|digest|budget|plen|perm。 */
#define STABLE_ITEM_LEN (1U + 32U + 4U + 1U + ENTELEA_TRIAL_PERM_MAX)
#define PERSIST_REC_LEN \
	(TRIAL_REC_LEN + ENTELEA_APP_SLOT_COUNT * STABLE_ITEM_LEN)

typedef struct {
	bool present;
	uint8_t digest[32];
	entelea_trial_grant_t grant;
} stable_entry_t;

static stable_entry_t stable[ENTELEA_APP_SLOT_COUNT];

static void
put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

static uint32_t
get_le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

entelea_result_t
entelea_trial_flush(void)
{
	uint8_t rec[PERSIST_REC_LEN];
	uint8_t verify[PERSIST_REC_LEN];
	uint16_t verify_len = 0;
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN) {
		return ENTELEA_E_STATE;
	}

	memset(rec, 0, sizeof(rec));
	rec[0] = TRIAL_PERSIST_VER;
	rec[1] = (uint8_t) state;
	rec[2] = current.version;
	rec[3] = current.slot;
	put_le32(rec + 4, last_epoch);
	put_le32(rec + 8, current.budget_steps);
	rec[12] = current.perm_len;
	memcpy(rec + TRIAL_REC_PERM_OFF, current.perm, current.perm_len);
	memcpy(rec + TRIAL_REC_DIG_OFF, current.digest, sizeof(current.digest));
	for (uint8_t i = 0; i < ENTELEA_APP_SLOT_COUNT; i++) {
		uint8_t *p = rec + TRIAL_REC_LEN + i * STABLE_ITEM_LEN;

		p[0] = stable[i].present ? 1U : 0U;
		memcpy(p + 1, stable[i].digest, sizeof(stable[i].digest));
		put_le32(p + 33, stable[i].grant.budget_steps);
		p[37] = stable[i].grant.perm_len;
		memcpy(p + 38, stable[i].grant.perm, stable[i].grant.perm_len);
	}
	r = entelea_storage_meta_write(rec, sizeof(rec));
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_meta_write(rec, sizeof(rec));
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_meta_read_mirrored(verify, sizeof(verify),
					       &verify_len);
	if (r != ENTELEA_OK || verify_len != sizeof(rec) ||
	    memcmp(rec, verify, sizeof(rec)) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

/* ---- 稳定记录（D-E）：每槽一项，与试用同一双副本 meta 记录原子持久。 */

entelea_result_t
entelea_trial_promote(const entelea_trial_candidate_t *shown)
{
	if (shown == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_STATE;
	}
	if (!candidate_matches(shown)) {
		return ENTELEA_E_PERM;
	}
	const entelea_trial_candidate_t keep = current;
	const uint8_t slot = current.slot;
	const stable_entry_t old = stable[slot];

	stable[slot].present = true;
	memcpy(stable[slot].digest, current.digest, sizeof(current.digest));
	stable[slot].grant.budget_steps = current.budget_steps;
	stable[slot].grant.perm_len = current.perm_len;
	memcpy(stable[slot].grant.perm, current.perm, current.perm_len);
	clear_candidate();
	state = ENTELEA_TRIAL_NONE;
	r = entelea_trial_flush(); /* 试用结束与稳定项同一原子写入 */
	if (r != ENTELEA_OK) {
		current = keep;
		stable[slot] = old;
		state = ENTELEA_TRIAL_ACTIVE;
	}
	return r;
}

entelea_result_t
entelea_stable_revoke(uint8_t slot)
{
	entelea_result_t r;

	if (slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	r = ensure_initialized();
	if (r != ENTELEA_OK) {
		return r; /* 持久读取失败：状态已按 TIMEOUT 关闭，须维护恢复 */
	}
	if (state == ENTELEA_TRIAL_ACTIVE && current.slot == slot) {
		clear_candidate();
		state = ENTELEA_TRIAL_NONE;
	}
	/* 内存先撤销（fail-closed）；写入失败则结果未知，须重试。 */
	stable[slot].present = false;
	memset(stable[slot].digest, 0, sizeof(stable[slot].digest));
	memset(&stable[slot].grant, 0, sizeof(stable[slot].grant));
	return entelea_trial_flush();
}

entelea_result_t
entelea_stable_grant(uint8_t slot, const uint8_t digest[32],
		     entelea_trial_grant_t *out)
{
	if (digest == NULL || out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (!stable[slot].present || memcmp(digest, stable[slot].digest,
					    sizeof(stable[slot].digest)) != 0) {
		return ENTELEA_E_PERM;
	}
	*out = stable[slot].grant;
	return ENTELEA_OK;
}

static entelea_result_t
meta_is_erased(bool *erased)
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

entelea_result_t
entelea_trial_restore(void)
{
	uint8_t rec[PERSIST_REC_LEN];
	uint16_t len = 0;
	entelea_result_t r;

	if (initialized && !(state == ENTELEA_TRIAL_TIMEOUT &&
			     current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN)) {
		return ENTELEA_OK;
	}
	r = entelea_storage_meta_read_mirrored(rec, sizeof(rec), &len);

	clear_candidate();
	memset(stable, 0, sizeof(stable));
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
	const uint32_t ep = whole ? get_le32(rec + 4) : 0U;
	const uint32_t budget = whole ? get_le32(rec + 8) : 0U;
	const bool perm_ok =
		whole && rec[12] <= ENTELEA_TRIAL_PERM_MAX &&
		perm_valid((const char *) (rec + TRIAL_REC_PERM_OFF), rec[12]);
	bool stable_ok = whole;

	for (uint8_t i = 0; whole && i < ENTELEA_APP_SLOT_COUNT; i++) {
		const uint8_t *p = rec + TRIAL_REC_LEN + i * STABLE_ITEM_LEN;

		stable_ok = stable_ok && p[0] <= 1U &&
			    (p[0] == 0U ||
			     (budget_valid(get_le32(p + 33)) &&
			      p[37] <= ENTELEA_TRIAL_PERM_MAX &&
			      (p[37] == 0U ||
			       perm_valid((const char *) (p + 38), p[37]))));
	}
	const bool valid = stable_ok && rec[0] == TRIAL_PERSIST_VER &&
			   rec[1] <= ENTELEA_TRIAL_TIMEOUT &&
			   (rec[1] == ENTELEA_TRIAL_NONE ||
			    (rec[3] < ENTELEA_APP_SLOT_COUNT && ep != 0U &&
			     budget_valid(budget) && perm_ok));

	if (!valid) {
		/* 记录不自洽：候选槽不可知，两槽均受门拒。 */
		state = ENTELEA_TRIAL_TIMEOUT;
		current.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		return ENTELEA_E_IO;
	}
	if (ep > last_epoch) {
		last_epoch = ep; /* 单调：恢复不回退纪元计数 */
	}
	state = (entelea_trial_state_t) rec[1];
	if (state != ENTELEA_TRIAL_NONE) {
		current.version = rec[2];
		current.slot = rec[3];
		current.epoch = ep;
		current.budget_steps = budget;
		current.perm_len = rec[12];
		memcpy(current.perm, rec + TRIAL_REC_PERM_OFF, rec[12]);
		memcpy(current.digest, rec + TRIAL_REC_DIG_OFF,
		       sizeof(current.digest));
	}
	for (uint8_t i = 0; i < ENTELEA_APP_SLOT_COUNT; i++) {
		const uint8_t *p = rec + TRIAL_REC_LEN + i * STABLE_ITEM_LEN;

		stable[i].present = p[0] == 1U;
		if (stable[i].present) {
			memcpy(stable[i].digest, p + 1,
			       sizeof(stable[i].digest));
			stable[i].grant.budget_steps = get_le32(p + 33);
			stable[i].grant.perm_len = p[37];
			memcpy(stable[i].grant.perm, p + 38, p[37]);
		}
	}
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
/* 状态机测试接缝：直接接受调用方摘要，不验签（生产路径不得使用）。 */
entelea_result_t
entelea_trial_submit_with_digest_for_test(const entelea_trial_candidate_t *cand)
{
	if (cand == NULL || cand->slot >= ENTELEA_APP_SLOT_COUNT ||
	    !perm_valid(cand->perm, cand->perm_len)) {
		return ENTELEA_E_PARAM;
	}
	if (!budget_valid(cand->budget_steps)) {
		return ENTELEA_E_RANGE;
	}
	entelea_result_t r = admit_check(cand->slot);

	if (r != ENTELEA_OK) {
		return r;
	}
	admit_commit(cand);
	return ENTELEA_OK;
}

void
entelea_trial_test_reset(void)
{
	state = ENTELEA_TRIAL_NONE;
	clear_candidate();
	memset(stable, 0, sizeof(stable));
	last_epoch = 0;
	initialized = false;
}
#endif
