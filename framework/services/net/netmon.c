/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R06 网络机制面（迁自旧 net/linkmon.c + lease.c + cmd.c 幂等缓存语义；
 * 合同见本目录 contract.md）。假传输注入：依赖状态机消费调用方上报的
 * 观测（真实网络探测归后续真机批）；本文件不碰真实网络/云。
 * 数值出处：心跳 1000ms/6 失败/2 恢复（DEC-22/27）、租约 TTL 10s
 * （DEC-41）、幂等缓存 4 槽 LRU（DEC-40）、恢复防抖 2 连（D08 建议）。
 */
#include <string.h>
#include <entelea/netmon.h>
#include <entelea/prov.h>
#include <entelea/safety.h>

/* ---- ① 心跳监视 ---- */

static bool up;
static bool have_host_hb;
static uint64_t last_host_ms;
static uint32_t streak;

void entelea_net_hb_host(uint64_t now_ms)
{
	last_host_ms = now_ms;
	have_host_hb = true;
	streak++;
	if (!up && streak >= ENTELEA_NET_HB_RECOVER) {
		up = true;
		streak = 0;
		/* 部署模式守卫（CG2）：仅 orchestrated 把链路判定喂给安全态
		 * 迁移；standalone 仅观测（本地逻辑照常）。 */
		if (entelea_prov()->deploy_mode == ENTELEA_DEPLOY_ORCHESTRATED) {
			(void)entelea_safety_set_link(true);
		}
	}
}

void entelea_net_linkmon_tick(uint64_t now_ms)
{
	if (have_host_hb && up &&
	    (now_ms - last_host_ms) >
		    (uint64_t)ENTELEA_NET_HB_PERIOD_MS * ENTELEA_NET_HB_MISS_LIMIT) {
		up = false;
		streak = 0;
		if (entelea_prov()->deploy_mode == ENTELEA_DEPLOY_ORCHESTRATED) {
			(void)entelea_safety_set_link(false);
		}
	}
}

bool entelea_net_link_up(void)
{
	return up;
}

/* ---- ② 控制租约 ---- */

struct lease_state {
	uint32_t id;
	uint64_t expires_at_ms;
	char holder[ENTELEA_NET_LEASE_HOLDER_MAX];
};
static struct lease_state lease;
static uint32_t last_granted;

static bool lease_valid(uint64_t now_ms)
{
	return lease.id != 0 && now_ms < lease.expires_at_ms;
}

entelea_result_t entelea_net_lease_acquire(const char *holder, uint64_t now_ms,
					   uint32_t *id, uint64_t *expires_at_ms)
{
	if (holder == NULL || holder[0] == '\0' ||
	    strlen(holder) >= ENTELEA_NET_LEASE_HOLDER_MAX) {
		return ENTELEA_E_PARAM;
	}
	/* 本地席位激活期：强制失效视图（acquire 拒——不改在册状态）。 */
	if (entelea_safety_arb_owner() == ENTELEA_ARB_LOCAL) {
		return ENTELEA_E_STATE;
	}
	if (lease_valid(now_ms)) {
		if (strcmp(lease.holder, holder) == 0) {
			/* 续期幂等：id 不变，窗口自 now 顺延。 */
			lease.expires_at_ms = now_ms + ENTELEA_NET_LEASE_TTL_MS;
		} else {
			if (id != NULL) {
				*id = lease.id;
			}
			if (expires_at_ms != NULL) {
				*expires_at_ms = lease.expires_at_ms;
			}
			return ENTELEA_E_STATE; /* 他人持有（回填归因） */
		}
	} else {
		last_granted++; /* id 单调递增不复用 */
		lease.id = last_granted;
		lease.expires_at_ms = now_ms + ENTELEA_NET_LEASE_TTL_MS;
		strcpy(lease.holder, holder);
	}
	if (id != NULL) {
		*id = lease.id;
	}
	if (expires_at_ms != NULL) {
		*expires_at_ms = lease.expires_at_ms;
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_net_lease_release(const char *holder, uint64_t now_ms)
{
	if (holder == NULL || holder[0] == '\0') {
		return ENTELEA_E_PARAM;
	}
	if (!lease_valid(now_ms)) {
		return ENTELEA_OK; /* 幂等空操作 */
	}
	if (strcmp(lease.holder, holder) != 0) {
		return ENTELEA_E_STATE; /* 他人租约 */
	}
	lease.id = 0; /* 归还（过期/释放后重新授予必得新 id） */
	lease.holder[0] = '\0';
	return ENTELEA_OK;
}

bool entelea_net_lease_held_by(const char *holder, uint64_t now_ms)
{
	if (holder == NULL) {
		return false;
	}
	/* 本地席位激活期：held_by 恒 false（gated 写命令拒——视图语义）。 */
	if (entelea_safety_arb_owner() == ENTELEA_ARB_LOCAL) {
		return false;
	}
	return lease_valid(now_ms) && strcmp(lease.holder, holder) == 0;
}

/* ---- ③ 幂等缓存（LRU；unknown 非幂等不自动重试） ---- */

struct idem_slot {
	bool used;
	uint64_t stamp;
	char idem[ENTELEA_NET_IDEM_KEY_MAX + 1];
	uint8_t reply[ENTELEA_NET_IDEM_REPLY_MAX];
	size_t rlen;
};
static struct idem_slot idem_cache[ENTELEA_NET_IDEM_CACHE];
static uint64_t idem_clock;

static struct idem_slot *idem_find(const char *key)
{
	for (int i = 0; i < ENTELEA_NET_IDEM_CACHE; i++) {
		if (idem_cache[i].used && strcmp(idem_cache[i].idem, key) == 0) {
			return &idem_cache[i];
		}
	}
	return NULL;
}

static void idem_insert(const char *key, const uint8_t *reply, size_t rlen)
{
	if (rlen > ENTELEA_NET_IDEM_REPLY_MAX) {
		return; /* 回执预算即上限（不可达防御） */
	}
	struct idem_slot *s = NULL;

	for (int i = 0; i < ENTELEA_NET_IDEM_CACHE; i++) {
		if (!idem_cache[i].used) {
			s = &idem_cache[i];
			break;
		}
	}
	if (s == NULL) { /* 满则驱逐最旧（定容 LRU） */
		s = &idem_cache[0];
		for (int i = 1; i < ENTELEA_NET_IDEM_CACHE; i++) {
			if (idem_cache[i].stamp < s->stamp) {
				s = &idem_cache[i];
			}
		}
	}
	s->used = true;
	strcpy(s->idem, key);
	memcpy(s->reply, reply, rlen);
	s->rlen = rlen;
	s->stamp = ++idem_clock;
}

entelea_result_t entelea_net_idem_run(const char *idem_key,
				      const uint8_t *request, size_t req_len,
				      entelea_result_t (*exec)(const uint8_t *,
							       size_t,
							       uint8_t *, size_t *),
				      uint8_t *reply, size_t *reply_len,
				      bool *replayed)
{
	if (idem_key == NULL || exec == NULL || reply == NULL ||
	    reply_len == NULL || strlen(idem_key) > ENTELEA_NET_IDEM_KEY_MAX) {
		return ENTELEA_E_PARAM;
	}
	if (replayed != NULL) {
		*replayed = false;
	}
	struct idem_slot *hit = idem_find(idem_key);

	if (hit != NULL) {
		/* 命中：回放缓存回执不重执行。 */
		size_t n = hit->rlen;

		if (n > *reply_len) {
			return ENTELEA_E_RANGE; /* 调用方缓冲不足（如实） */
		}
		memcpy(reply, hit->reply, n);
		*reply_len = n;
		hit->stamp = ++idem_clock;
		if (replayed != NULL) {
			*replayed = true;
		}
		return ENTELEA_OK;
	}
	size_t n = *reply_len;
	entelea_result_t r = exec(request, req_len, reply, &n);

	/* 仅确定结果入缓存：PENDING/错误（含 unknown 效果）不缓存——
	 * 下次同 idem 重执行（调用方决策重试与否；本层不静默重试
	 * 也不把 unknown 冒充已确认）。 */
	if (r == ENTELEA_OK && n <= ENTELEA_NET_IDEM_REPLY_MAX) {
		idem_insert(idem_key, reply, n);
	}
	*reply_len = n;
	return r;
}

/* ---- ④ 依赖状态机 ---- */

#define DEP_RECOVER_STREAK 2U /* 恢复防抖（D08 建议：2 连 good） */

static struct {
	entelea_dep_state_t state;
	uint32_t good_streak;
	uint64_t last_change_ms;
} deps[ENTELEA_DEP_COUNT];

static const char *const dep_names[ENTELEA_DEP_COUNT] = {
	"network", "model", "storage", "app",
};

const char *entelea_dep_name(entelea_dep_kind_t kind)
{
	return (int)kind >= 0 && kind < ENTELEA_DEP_COUNT ? dep_names[kind] : "?";
}

entelea_dep_state_t entelea_dep_state(entelea_dep_kind_t kind)
{
	if ((int)kind < 0 || kind >= ENTELEA_DEP_COUNT) {
		return ENTELEA_DEP_DOWN;
	}
	return deps[kind].state;
}

entelea_result_t entelea_dep_report(entelea_dep_kind_t kind, bool good,
				    uint64_t now_ms)
{
	if ((int)kind < 0 || kind >= ENTELEA_DEP_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (good) {
		switch (deps[kind].state) {
		case ENTELEA_DEP_DOWN:
			deps[kind].good_streak++;
			if (deps[kind].good_streak >= DEP_RECOVER_STREAK) {
				deps[kind].state = ENTELEA_DEP_UP;
				deps[kind].good_streak = 0;
				deps[kind].last_change_ms = now_ms;
			} else {
				deps[kind].state = ENTELEA_DEP_DEGRADED;
			}
			break;
		case ENTELEA_DEP_DEGRADED:
			deps[kind].good_streak++;
			if (deps[kind].good_streak >= DEP_RECOVER_STREAK) {
				deps[kind].state = ENTELEA_DEP_UP;
				deps[kind].good_streak = 0;
				deps[kind].last_change_ms = now_ms;
			}
			break;
		case ENTELEA_DEP_UP:
			break; /* 已 UP（无变化） */
		}
	} else {
		/* 失败侧保守：立即 DOWN（防抖只在恢复侧——风暴抑制）。 */
		if (deps[kind].state != ENTELEA_DEP_DOWN) {
			deps[kind].last_change_ms = now_ms;
		}
		deps[kind].state = ENTELEA_DEP_DOWN;
		deps[kind].good_streak = 0;
	}
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_net_test_reset(void)
{
	up = false;
	have_host_hb = false;
	last_host_ms = 0;
	streak = 0;
	memset(&lease, 0, sizeof(lease));
	last_granted = 0;
	memset(idem_cache, 0, sizeof(idem_cache));
	idem_clock = 0;
	memset(deps, 0, sizeof(deps));
}
#endif
