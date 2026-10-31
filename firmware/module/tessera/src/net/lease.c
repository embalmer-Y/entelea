/* SPDX-License-Identifier: Apache-2.0 */
/* 控制租约（LLD-ts-net §4.5，DEC-41）：多方并发命令的准入仲裁。
 * V1 单租约：TTL 默认 10s〔DEC-41：≈断链窗口 6s（DEC-22）×1.5+余量〕；
 * 惰性过期（访问时判定，无定时器——合同 9 确定性）；lease_id 单调递增不复用
 * （过期/归还后重新授予必得新 id）。只管命令准入，不联动安全态——输出安全态
 * 唯一判定源仍是 linkmon/estop（合同 3/5 单源纪律）。上下文：命令分发
 * （串行）与测试直调，无锁。
 * CG3（Q-41②/合同 11 · LLD-ts-safety §5b 租约联动）：本地席位激活期
 * （ts_safety arb_owner == LOCAL）= 租约**强制失效视图**——held_by 恒
 * false（gated 写命令拒）+ acquire 拒（TS_E_STATE——回执由 cmd 层装饰
 * local_active 提示）；本地释放（set_arb(REMOTE)）= 远程可重新 acquire。
 * 不改写租约在册状态（视图语义——本地激活期 TTL 照常走完，释放后旧租约
 * 若未过期仍有效，幂等续期不变）。 */
#include <string.h>
#include <ts/net.h>
#include <ts/safety.h>
#include "internal.h"

struct lease_state {
	uint32_t id;              /* 0 = 无租约在册 */
	uint64_t expires_at_ms;
	char holder[TS_NET_LEASE_HOLDER_MAX];
};

static struct lease_state lease;
static uint32_t last_granted; /* 已授予的最大 id（不复用） */

static bool lease_valid(uint64_t now_ms)
{
	return lease.id != 0 && now_ms < lease.expires_at_ms;
}

ts_res_t ts_net_lease_acquire(const char *holder, uint64_t now_ms,
			      uint32_t *id, uint64_t *expires_at_ms)
{
	if (holder == NULL || holder[0] == '\0' || strlen(holder) >= TS_NET_LEASE_HOLDER_MAX) {
		return TS_E_PARAM;
	}
	/* CG3 · 本地席位激活期：acquire 拒（降级视图——含同 holder 续期；
	 * 不改在册状态，见文件头注） */
	if (ts_safety_arb_owner() == TS_ARB_LOCAL) {
		return TS_E_STATE;
	}
	if (lease_valid(now_ms)) {
		if (strcmp(lease.holder, holder) == 0) {
			/* 续期（re-acquire 幂等）：id 不变，窗口自 now 顺延 */
			lease.expires_at_ms = now_ms + CONFIG_TS_NET_LEASE_TTL_MS;
		} else {
			/* 他人持有 → 拒绝（回填当前租约供拒绝回执归因） */
			if (id != NULL) {
				*id = lease.id;
			}
			if (expires_at_ms != NULL) {
				*expires_at_ms = lease.expires_at_ms;
			}
			return TS_E_STATE;
		}
	} else {
		last_granted++;
		lease.id = last_granted;
		lease.expires_at_ms = now_ms + CONFIG_TS_NET_LEASE_TTL_MS;
		strcpy(lease.holder, holder);
	}
	if (id != NULL) {
		*id = lease.id;
	}
	if (expires_at_ms != NULL) {
		*expires_at_ms = lease.expires_at_ms;
	}
	return TS_OK;
}

ts_res_t ts_net_lease_release(const char *holder, uint64_t now_ms)
{
	if (holder == NULL || holder[0] == '\0') {
		return TS_E_PARAM;
	}
	if (!lease_valid(now_ms)) {
		return TS_OK; /* 无有效租约 = 幂等空操作 */
	}
	if (strcmp(lease.holder, holder) != 0) {
		return TS_E_STATE; /* 他人持有，不可代还 */
	}
	lease.id = 0;
	lease.holder[0] = '\0';
	return TS_OK;
}

void ts_net_lease_get(uint64_t now_ms, bool *valid, char *holder, size_t holder_cap,
		      uint32_t *id, uint64_t *expires_at_ms)
{
	bool v = lease_valid(now_ms);

	if (valid != NULL) {
		*valid = v;
	}
	if (holder != NULL && holder_cap > 0) {
		if (v) {
			strncpy(holder, lease.holder, holder_cap - 1);
			holder[holder_cap - 1] = '\0';
		} else {
			holder[0] = '\0';
		}
	}
	if (id != NULL) {
		*id = v ? lease.id : 0;
	}
	if (expires_at_ms != NULL) {
		*expires_at_ms = v ? lease.expires_at_ms : 0;
	}
}

bool ts_net_lease_held_by(const char *holder, uint64_t now_ms)
{
	/* CG3：本地席位激活期 = 租约强制失效视图（gated 写命令拒——与
	 * commit 入口的来源压制〔TS_SRC_ZENOH〕构成双层闸：租约面拦命令
	 * 准入，安全层拦任何漏至写路径的远程来源） */
	if (ts_safety_arb_owner() == TS_ARB_LOCAL) {
		return false;
	}
	return holder != NULL && lease_valid(now_ms) &&
	       strcmp(lease.holder, holder) == 0;
}

#ifdef CONFIG_TS_TEST
void ts_net_lease_test_reset(void)
{
	memset(&lease, 0, sizeof(lease));
	last_granted = 0;
}
#endif
