/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_NETMON_H
#define ENTELEA_NETMON_H

/* R06 network and dynamic dependency contract: see
 * framework/services/net/contract.md.
 * Mechanisms plus a fake transport (no real network or cloud access). There
 * are four sub-surfaces:
 * 1. Heartbeat monitoring (link-down decision source, recovery hysteresis, and
 *    deployment-mode guard);
 * 2. Control leases (admission arbitration, lazy expiry, IDs are never reused,
 *    local seat view invalidation);
 * 3. Idempotency cache (an idem hit replays the cached receipt without
 *    re-executing; unknown outcomes of non-idempotent actions are not retried
 *    automatically);
 * 4. Dependency state machine (network/model/storage/App declarations ->
 *    observable dependency states; a standalone App is not tied to the global
 *    assistant state). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. Heartbeat (legacy linkmon semantics; DEC-22/27) ---- */

#define ENTELEA_NET_HB_PERIOD_MS 1000U
#define ENTELEA_NET_HB_MISS_LIMIT 6U
#define ENTELEA_NET_HB_RECOVER 2U

/** [thread] Host heartbeat arrives (streak++). Recovery is hysteretic: the link
 * goes up only when the streak is >= RECOVER. */
void
entelea_net_hb_host(uint64_t now_ms);

/** [thread] Periodic check. On timeout (elapsed > MISS_LIMIT * PERIOD),
 * the link goes down. */
void
entelea_net_linkmon_tick(uint64_t now_ms);

/** [any] Mirror of the decided link state. */
bool
entelea_net_link_up(void);

/* ---- 2. Control leases (legacy lease semantics; TTL from DEC-41) ---- */

#define ENTELEA_NET_LEASE_TTL_MS 10000U
#define ENTELEA_NET_LEASE_HOLDER_MAX 24

/** [thread] Acquires or renews a lease. Renewal by the same holder is
 * idempotent and keeps the id. If another holder owns the lease, the request
 * is refused and the current lease attribution is returned. During local seat
 * activation the view is forcibly invalidated, and requests are refused.
 * Expiry is lazy (no timer). lease_id increases monotonically and is never
 * reused. */
entelea_result_t
entelea_net_lease_acquire(const char *holder, uint64_t now_ms, uint32_t *id,
			  uint64_t *expires_at_ms);

/** [thread] Releases a lease. With no valid lease this is an idempotent no-op;
 * releasing another holder's lease is refused. */
entelea_result_t
entelea_net_lease_release(const char *holder, uint64_t now_ms);

/** [any] Whether the given holder holds a valid lease (always false while the
 * local seat is active; this is a view). */
bool
entelea_net_lease_held_by(const char *holder, uint64_t now_ms);

/* ---- 3. Idempotency cache (legacy DEC-40 LRU semantics) ---- */

#define ENTELEA_NET_IDEM_CACHE 4
#define ENTELEA_NET_IDEM_KEY_MAX 16
#define ENTELEA_NET_IDEM_REPLY_MAX 128

/** Idempotent execution wrapper. On an idem hit, the cached receipt is
 * replayed without re-executing (replayed=true). On a miss, exec is called and
 * the result is cached only if exec returns OK with a non-unknown effect.
 * **Unknown outcomes of non-idempotent actions are never retried
 * automatically**: when exec returns PENDING or an unknown effect, nothing is
 * cached. The next call with the same idem key re-executes; that is the
 * caller's decision, and this layer neither retries silently nor swallows the
 * result. */
entelea_result_t
entelea_net_idem_run(const char *idem_key, const uint8_t *request,
		     size_t req_len,
		     entelea_result_t (*exec)(const uint8_t *, size_t,
					      uint8_t *, size_t *),
		     uint8_t *reply, size_t *reply_len, bool *replayed);

/* ---- 4. Dependency state machine (driven by real changes) ---- */

typedef enum {
	ENTELEA_DEP_DOWN = 0,
	ENTELEA_DEP_DEGRADED, /* usable but flapping (recovery debounce) */
	ENTELEA_DEP_UP,
} entelea_dep_state_t;

typedef enum {
	ENTELEA_DEP_NET = 0,
	ENTELEA_DEP_MODEL,
	ENTELEA_DEP_STORAGE,
	ENTELEA_DEP_APP,
	ENTELEA_DEP_COUNT,
} entelea_dep_kind_t;

/** [thread] Reports a raw observation (transport probe, cloud call result,
 * storage IO, or App declaration). Recovery debounce: DOWN -> UP needs
 * good_streak consecutive good observations (default 2, which suppresses
 * storms). UP -> DOWN is immediate (the failure side is conservative).
 * Dependency state and read-only queries are synchronized; other network
 * operations outside the dependency state machine still require caller
 * serialization. */
entelea_result_t
entelea_dep_report(entelea_dep_kind_t kind, bool good, uint64_t now_ms);

/** [thread] Marks a dependency as unavailable. No observation time is
 * fabricated. */
entelea_result_t
entelea_dep_mark_down(entelea_dep_kind_t kind);

/** [any] Current dependency state. This is a thread- and ISR-safe snapshot
 * that does not lock later operations. */
entelea_dep_state_t
entelea_dep_state(entelea_dep_kind_t kind);

/** Dependency name (for observation and audit attribution). */
const char *
entelea_dep_name(entelea_dep_kind_t kind);

#ifdef ENTELEA_TEST
void
entelea_net_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_NETMON_H */
