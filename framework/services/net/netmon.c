/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R06 network mechanism surface (migrated from the old net/linkmon.c,
 * lease.c and cmd.c idempotent-cache semantics; see contract.md in this
 * directory). Fake transport injection: the dependency state machine
 * consumes observations reported by the caller (real network probing is
 * deferred to later real-device batches); this file never touches the real
 * network or cloud.
 * Value sources: heartbeat 1000ms / 6 misses / 2 recoveries (DEC-22/27),
 * lease TTL 10s (DEC-41), 4-slot LRU idempotent cache (DEC-40), recovery
 * debounce of 2 consecutive (D08 advice).
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/netmon.h>
#include <entelea/prov.h>
#include <entelea/safety.h>

/* ---- 1. Heartbeat monitor ---- */

static bool up;
static bool have_host_hb;
static uint64_t last_host_ms;
static uint32_t streak;

void
entelea_net_hb_host(uint64_t now_ms)
{
	last_host_ms = now_ms;
	have_host_hb = true;
	streak++;
	if (!up && streak >= ENTELEA_NET_HB_RECOVER) {
		up = true;
		streak = 0;
		/* Deploy-mode guard (CG2): only orchestrated feeds link
		 * decisions into safety-state transitions; standalone only
		 * observes (local logic continues as usual). */
		if (entelea_prov()->deploy_mode ==
		    ENTELEA_DEPLOY_ORCHESTRATED) {
			(void) entelea_safety_set_link(true);
		}
	}
}

void
entelea_net_linkmon_tick(uint64_t now_ms)
{
	if (have_host_hb && up &&
	    (now_ms - last_host_ms) > (uint64_t) ENTELEA_NET_HB_PERIOD_MS *
					      ENTELEA_NET_HB_MISS_LIMIT) {
		up = false;
		streak = 0;
		if (entelea_prov()->deploy_mode ==
		    ENTELEA_DEPLOY_ORCHESTRATED) {
			(void) entelea_safety_set_link(false);
		}
	}
}

bool
entelea_net_link_up(void)
{
	return up;
}

/* ---- 2. Control lease ---- */

struct lease_state {
	uint32_t id;
	uint64_t expires_at_ms;
	char holder[ENTELEA_NET_LEASE_HOLDER_MAX];
};
static struct lease_state lease;
static uint32_t last_granted;

static bool
lease_valid(uint64_t now_ms)
{
	return lease.id != 0 && now_ms < lease.expires_at_ms;
}

entelea_result_t
entelea_net_lease_acquire(const char *holder, uint64_t now_ms, uint32_t *id,
			  uint64_t *expires_at_ms)
{
	if (holder == NULL || holder[0] == '\0' ||
	    strlen(holder) >= ENTELEA_NET_LEASE_HOLDER_MAX) {
		return ENTELEA_E_PARAM;
	}
	/* Local seat active: the view is forced invalid (acquire is refused;
	 * the registered state is unchanged). */
	if (entelea_safety_arb_owner() == ENTELEA_ARB_LOCAL) {
		return ENTELEA_E_STATE;
	}
	if (lease_valid(now_ms)) {
		if (strcmp(lease.holder, holder) == 0) {
			/* Renewal idempotent: same id, window from now */
			lease.expires_at_ms = now_ms + ENTELEA_NET_LEASE_TTL_MS;
		} else {
			if (id != NULL) {
				*id = lease.id;
			}
			if (expires_at_ms != NULL) {
				*expires_at_ms = lease.expires_at_ms;
			}
			return ENTELEA_E_STATE; /* Held by another party */
		}
	} else {
		last_granted++; /* id: monotonic, never reused */
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

entelea_result_t
entelea_net_lease_release(const char *holder, uint64_t now_ms)
{
	if (holder == NULL || holder[0] == '\0') {
		return ENTELEA_E_PARAM;
	}
	if (!lease_valid(now_ms)) {
		return ENTELEA_OK; /* Idempotent no-op */
	}
	if (strcmp(lease.holder, holder) != 0) {
		return ENTELEA_E_STATE; /* Another holder's lease */
	}
	lease.id = 0; /* Returned: re-grant always gets a new id */
	lease.holder[0] = '\0';
	return ENTELEA_OK;
}

bool
entelea_net_lease_held_by(const char *holder, uint64_t now_ms)
{
	if (holder == NULL) {
		return false;
	}
	/* Local seat active: held_by is always false (gated write
	 * commands are refused; view semantics). */
	if (entelea_safety_arb_owner() == ENTELEA_ARB_LOCAL) {
		return false;
	}
	return lease_valid(now_ms) && strcmp(lease.holder, holder) == 0;
}

/* ---- 3. Idempotent cache (LRU; unknown non-idempotent: no retry) ---- */

struct idem_slot {
	bool used;
	uint64_t stamp;
	char idem[ENTELEA_NET_IDEM_KEY_MAX + 1];
	uint8_t reply[ENTELEA_NET_IDEM_REPLY_MAX];
	size_t rlen;
};
static struct idem_slot idem_cache[ENTELEA_NET_IDEM_CACHE];
static uint64_t idem_clock;

static struct idem_slot *
idem_find(const char *key)
{
	for (int i = 0; i < ENTELEA_NET_IDEM_CACHE; i++) {
		if (idem_cache[i].used &&
		    strcmp(idem_cache[i].idem, key) == 0) {
			return &idem_cache[i];
		}
	}
	return NULL;
}

static void
idem_insert(const char *key, const uint8_t *reply, size_t rlen)
{
	if (rlen > ENTELEA_NET_IDEM_REPLY_MAX) {
		return; /* Reply budget is the cap (unreachable guard) */
	}
	struct idem_slot *s = NULL;

	for (int i = 0; i < ENTELEA_NET_IDEM_CACHE; i++) {
		if (!idem_cache[i].used) {
			s = &idem_cache[i];
			break;
		}
	}
	if (s == NULL) { /* Full: evict oldest (fixed-size LRU) */
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

entelea_result_t
entelea_net_idem_run(const char *idem_key, const uint8_t *request,
		     size_t req_len,
		     entelea_result_t (*exec)(const uint8_t *, size_t,
					      uint8_t *, size_t *),
		     uint8_t *reply, size_t *reply_len, bool *replayed)
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
		/* Hit: replay the cached receipt, do not re-execute. */
		size_t n = hit->rlen;

		if (n > *reply_len) {
			return ENTELEA_E_RANGE; /* Buffer too small; reported */
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

	/* Only definite results are cached. PENDING and errors (including
	 * unknown effects) are not cached: the same idem key re-executes
	 * next attempt (the caller decides whether to retry; this layer neither
	 * retries silently nor passes off unknown as confirmed). */
	if (r == ENTELEA_OK && n <= ENTELEA_NET_IDEM_REPLY_MAX) {
		idem_insert(idem_key, reply, n);
	}
	*reply_len = n;
	return r;
}

/* ---- 4. Dependency state machine ---- */

#define DEP_RECOVER_STREAK 2U /* Recovery debounce (D08: 2 good in a row) */

static struct {
	entelea_dep_state_t state;
	uint32_t good_streak;
	uint64_t last_change_ms;
	bool last_change_known;
} deps[ENTELEA_DEP_COUNT];
static struct k_spinlock deps_lock;

static const char *const dep_names[ENTELEA_DEP_COUNT] = {
	"network",
	"model",
	"storage",
	"app",
};

const char *
entelea_dep_name(entelea_dep_kind_t kind)
{
	return (int) kind >= 0 && kind < ENTELEA_DEP_COUNT ? dep_names[kind] :
							     "?";
}

entelea_dep_state_t
entelea_dep_state(entelea_dep_kind_t kind)
{
	entelea_dep_state_t state;
	k_spinlock_key_t key;

	if ((int) kind < 0 || kind >= ENTELEA_DEP_COUNT) {
		return ENTELEA_DEP_DOWN;
	}
	key = k_spin_lock(&deps_lock);
	state = deps[kind].state;
	k_spin_unlock(&deps_lock, key);
	return state;
}

entelea_result_t
entelea_dep_report(entelea_dep_kind_t kind, bool good, uint64_t now_ms)
{
	k_spinlock_key_t key;

	if ((int) kind < 0 || kind >= ENTELEA_DEP_COUNT) {
		return ENTELEA_E_PARAM;
	}
	key = k_spin_lock(&deps_lock);
	if (good) {
		switch (deps[kind].state) {
		case ENTELEA_DEP_DOWN:
			deps[kind].good_streak++;
			if (deps[kind].good_streak >= DEP_RECOVER_STREAK) {
				deps[kind].state = ENTELEA_DEP_UP;
				deps[kind].good_streak = 0;
				deps[kind].last_change_ms = now_ms;
				deps[kind].last_change_known = true;
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
				deps[kind].last_change_known = true;
			}
			break;
		case ENTELEA_DEP_UP:
			break; /* Already UP: no change */
		}
	} else {
		/* Failures are conservative: go DOWN at once (debounce only
		 * applies on recovery, to suppress storms). */
		if (deps[kind].state != ENTELEA_DEP_DOWN) {
			deps[kind].last_change_ms = now_ms;
			deps[kind].last_change_known = true;
		}
		deps[kind].state = ENTELEA_DEP_DOWN;
		deps[kind].good_streak = 0;
	}
	k_spin_unlock(&deps_lock, key);
	return ENTELEA_OK;
}

entelea_result_t
entelea_dep_mark_down(entelea_dep_kind_t kind)
{
	k_spinlock_key_t key;

	if ((int) kind < 0 || kind >= ENTELEA_DEP_COUNT) {
		return ENTELEA_E_PARAM;
	}
	key = k_spin_lock(&deps_lock);
	if (deps[kind].state != ENTELEA_DEP_DOWN) {
		deps[kind].last_change_known = false;
	}
	deps[kind].state = ENTELEA_DEP_DOWN;
	deps[kind].good_streak = 0;
	k_spin_unlock(&deps_lock, key);
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void
entelea_net_test_reset(void)
{
	k_spinlock_key_t key;

	up = false;
	have_host_hb = false;
	last_host_ms = 0;
	streak = 0;
	memset(&lease, 0, sizeof(lease));
	last_granted = 0;
	memset(idem_cache, 0, sizeof(idem_cache));
	idem_clock = 0;
	key = k_spin_lock(&deps_lock);
	memset(deps, 0, sizeof(deps));
	k_spin_unlock(&deps_lock, key);
}
#endif
