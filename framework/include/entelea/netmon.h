/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_NETMON_H
#define ENTELEA_NETMON_H

/* R06 网络/动态依赖合同见 framework/services/net/contract.md。
 * 机制+假传输（不碰真实网络/云）。四子面：
 * ①心跳监视（断链判定源+恢复滞回+部署模式守卫）；
 * ②控制租约（准入仲裁，惰性过期，id 不复用，本地席位视图失效）；
 * ③幂等缓存（idem 命中回放缓存回执不重执行；unknown 非幂等不自动重试）；
 * ④依赖状态机（network/model/storage/App 声明→可观测依赖态→
 *   独立 App 不被全局智脑状态绑死）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ① 心跳监视（旧 linkmon 语义；数值出处 DEC-22/27） ---- */

#define ENTELEA_NET_HB_PERIOD_MS 1000U
#define ENTELEA_NET_HB_MISS_LIMIT 6U
#define ENTELEA_NET_HB_RECOVER 2U

/** [thread] host 心跳到达（streak++；恢复滞回 ≥RECOVER 才 up）。 */
void entelea_net_hb_host(uint64_t now_ms);

/** [thread] 周期判定：超时（elapsed > MISS_LIMIT×PERIOD）→ down。 */
void entelea_net_linkmon_tick(uint64_t now_ms);

/** [any] 判定状态镜像。 */
bool entelea_net_link_up(void);

/* ---- ② 控制租约（旧 lease 语义；TTL 出处 DEC-41） ---- */

#define ENTELEA_NET_LEASE_TTL_MS 10000U
#define ENTELEA_NET_LEASE_HOLDER_MAX 24

/** [thread] 获取/续期（同 holder 幂等续期 id 不变；他人持有拒并回填
 * 当前租约归因；本地席位激活期=强制失效视图拒）。惰性过期（无定时器）。
 * lease_id 单调递增不复用。 */
entelea_result_t entelea_net_lease_acquire(const char *holder, uint64_t now_ms,
					   uint32_t *id, uint64_t *expires_at_ms);

/** [thread] 归还（无有效租约 = 幂等空操作；他人租约拒）。 */
entelea_result_t entelea_net_lease_release(const char *holder, uint64_t now_ms);

/** [any] 指定 holder 是否持有有效租约（本地席位激活期恒 false——视图）。 */
bool entelea_net_lease_held_by(const char *holder, uint64_t now_ms);

/* ---- ③ 幂等缓存（旧 DEC-40 LRU 语义） ---- */

#define ENTELEA_NET_IDEM_CACHE 4
#define ENTELEA_NET_IDEM_KEY_MAX 16
#define ENTELEA_NET_IDEM_REPLY_MAX 128

/** 幂等执行包装：idem 命中 → 回放缓存回执不重执行（replayed=true）；
 * 未命中 → 调 exec 并缓存（仅当 exec 返回 OK 且非 unknown 效果）。
 * **unknown 非幂等不自动重试**：exec 返回 PENDING/unknown 效果时不
 * 缓存（下次同 idem 重执行——调用方决策，本层不静默重试也不吞结果）。 */
entelea_result_t entelea_net_idem_run(const char *idem_key,
				      const uint8_t *request, size_t req_len,
				      entelea_result_t (*exec)(const uint8_t *,
							       size_t,
							       uint8_t *, size_t *),
				      uint8_t *reply, size_t *reply_len,
				      bool *replayed);

/* ---- ④ 依赖状态机（真实依赖变化驱动准入/暂停/恢复） ---- */

typedef enum {
	ENTELEA_DEP_DOWN = 0,
	ENTELEA_DEP_DEGRADED, /* 可用但抖动（恢复防抖窗内） */
	ENTELEA_DEP_UP,
} entelea_dep_state_t;

typedef enum {
	ENTELEA_DEP_NET = 0,
	ENTELEA_DEP_MODEL,
	ENTELEA_DEP_STORAGE,
	ENTELEA_DEP_APP,
	ENTELEA_DEP_COUNT,
} entelea_dep_kind_t;

/** [thread] 上报原始观测（transport 探测/云调用结果/存储 IO/App 声明）。
 * 恢复防抖：DOWN→UP 需连续 good_streak 次 good 观测（默认 2——风暴
 * 抑制）；UP→DOWN 立即（失败侧保守）。 */
entelea_result_t entelea_dep_report(entelea_dep_kind_t kind, bool good,
				    uint64_t now_ms);

/** [any] 依赖当前态（可观测：观测者/命令准入/独立 App 判各自依赖）。 */
entelea_dep_state_t entelea_dep_state(entelea_dep_kind_t kind);

/** 依赖名（观测/审计归因用）。 */
const char *entelea_dep_name(entelea_dep_kind_t kind);

#ifdef ENTELEA_TEST
void entelea_net_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_NETMON_H */
