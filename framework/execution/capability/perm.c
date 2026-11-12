/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Capability grammar parsing and caller authorization (migrated from the
 * authorization surface of the old hal/perm.c; R04 contract:
 * framework/execution/capability/contract.md).
 * Principal binding slots are the trusted host side (mapped on the native
 * side; APPs cannot address them). Unauthorized access, mismatched fields,
 * or stale handles are rejected and logged (EVT_PERM_DENIED, observation
 * surface).
 * Relative to the old implementation: classes and operation sets are trimmed
 * to the R04 consumer surface (mainly gpio/pwm/power/msg). Other classes only
 * register grammar; ops without a consumer grant no new semantics, so
 * fail-closed is kept.
 */
#include <ctype.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/events.h>
#include <entelea/principal.h>

#define MAX_BOUND_CTX 4 /* Max APPs loaded at once (DEC-27 #10) */

struct bound_ctx {
	uint16_t app_id;
	uint16_t gen; /* Last bind generation; kept on unbind (ABA guard) */
	entelea_perm_table_t table;
	bool used;
};
static struct bound_ctx contexts[MAX_BOUND_CTX];
static struct k_spinlock contexts_lock;

#ifdef ENTELEA_TEST
static void (*resolve_hook)(void);

void
entelea_perm_test_after_resolve(void (*hook)(void))
{
	resolve_hook = hook;
}
#endif

void
entelea_perm_table_init(entelea_perm_table_t *table)
{
	if (table != NULL) {
		memset(table, 0, sizeof(*table));
	}
}

static int
parse_class(const char *s, size_t len)
{
	if (len == 4 && strncmp(s, "gpio", 4) == 0)
		return ENTELEA_PERM_GPIO;
	if (len == 3 && strncmp(s, "pwm", 3) == 0)
		return ENTELEA_PERM_PWM;
	if (len == 3 && strncmp(s, "adc", 3) == 0)
		return ENTELEA_PERM_ADC;
	if (len == 5 && strncmp(s, "power", 5) == 0)
		return ENTELEA_PERM_POWER;
	if (len == 2 && strncmp(s, "fs", 2) == 0)
		return ENTELEA_PERM_FS;
	if (len == 2 && strncmp(s, "av", 2) == 0)
		return ENTELEA_PERM_AV;
	if (len == 3 && strncmp(s, "log", 3) == 0)
		return ENTELEA_PERM_LOG;
	if (len == 2 && strncmp(s, "kv", 2) == 0)
		return ENTELEA_PERM_KV;
	if (len == 3 && strncmp(s, "msg", 3) == 0)
		return ENTELEA_PERM_MSG;
	if (len == 3 && strncmp(s, "sys", 3) == 0)
		return ENTELEA_PERM_SYS;
	return -1;
}

static int
parse_op(const char *s, size_t len)
{
	if (len == 4 && strncmp(s, "read", 4) == 0)
		return ENTELEA_PERM_READ;
	if (len == 5 && strncmp(s, "write", 5) == 0)
		return ENTELEA_PERM_WRITE;
	if (len == 3 && strncmp(s, "set", 3) == 0)
		return ENTELEA_PERM_SET;
	if (len == 4 && strncmp(s, "list", 4) == 0)
		return ENTELEA_PERM_LIST;
	if (len == 6 && strncmp(s, "delete", 6) == 0)
		return ENTELEA_PERM_DELETE;
	if (len == 4 && strncmp(s, "send", 4) == 0)
		return ENTELEA_PERM_SEND;
	return -1;
}

/* msg name grammar: [A-Za-z0-9._-]{1,39}, the same allowlist as zenoh
 * key segments. */
static bool
valid_msg_name(const char *s, size_t len)
{
	if (len == 0 || len >= ENTELEA_PERM_MSG_NAME_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
			  c == '-';

		if (!ok) {
			return false;
		}
	}
	return true;
}

static entelea_result_t
msg_target_add(entelea_perm_table_t *table, const char *s, size_t len)
{
	char name[ENTELEA_PERM_MSG_NAME_MAX];

	memcpy(name, s, len);
	name[len] = '\0';
	for (int i = 0; i < ENTELEA_PERM_MSG_TARGETS_MAX; i++) {
		if (table->msg_targets[i][0] == '\0') {
			strcpy(table->msg_targets[i], name);
			return ENTELEA_OK;
		}
		if (strcmp(table->msg_targets[i], name) == 0) {
			return ENTELEA_OK; /* Duplicate is idempotent */
		}
	}
	return ENTELEA_E_NOMEM; /* Full at load time: reject, no truncation */
}

/* Instance bitmap: "0-3" / "0,2" / "5"; instances above 31 are
 * rejected. */
static entelea_result_t
parse_instances(const char *s, size_t len, uint32_t *bitmap)
{
	size_t i = 0;

	while (i < len) {
		if (s[i] == ',') {
			i++;
			continue;
		}
		if (!isdigit((int) s[i])) {
			return ENTELEA_E_PARAM;
		}
		uint32_t start = 0, end = 0;

		while (i < len && isdigit((int) s[i])) {
			start = start * 10 + (uint32_t) (s[i] - '0');
			if (start > 31) {
				return ENTELEA_E_PARAM;
			}
			i++;
		}
		end = start;
		if (i < len && s[i] == '-') {
			i++;
			if (i >= len || !isdigit((int) s[i])) {
				return ENTELEA_E_PARAM;
			}
			end = 0;
			while (i < len && isdigit((int) s[i])) {
				end = end * 10 + (uint32_t) (s[i] - '0');
				if (end > 31) {
					return ENTELEA_E_PARAM;
				}
				i++;
			}
		}
		if (end < start) {
			return ENTELEA_E_PARAM;
		}
		for (uint32_t b = start; b <= end; b++) {
			*bitmap |= (1U << b);
		}
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_perm_parse(const char *cap, entelea_perm_table_t *table)
{
	if (cap == NULL || table == NULL) {
		return ENTELEA_E_PARAM;
	}
	const char *c1 = strchr(cap, ':');

	if (c1 == NULL) {
		return ENTELEA_E_PARAM;
	}
	const char *c2 = strchr(c1 + 1, ':');

	if (c2 == NULL) {
		return ENTELEA_E_PARAM;
	}
	int cls = parse_class(cap, (size_t) (c1 - cap));

	if (cls < 0) {
		return ENTELEA_E_PARAM;
	}
	int op = parse_op(c1 + 1, (size_t) (c2 - c1 - 1));

	if (op < 0) {
		return ENTELEA_E_PARAM;
	}
	/* msg class uses the allowlist form (op must be send; recv is
	 * passive with no permission gate; other ops have no consumer, so
	 * fail-closed). */
	if (cls == ENTELEA_PERM_MSG) {
		size_t tlen = strlen(c2 + 1);

		if (op != ENTELEA_PERM_SEND || !valid_msg_name(c2 + 1, tlen)) {
			return ENTELEA_E_PARAM;
		}
		return msg_target_add(table, c2 + 1, tlen);
	}
	uint32_t bitmap = 0;
	entelea_result_t r = parse_instances(c2 + 1, strlen(c2 + 1), &bitmap);

	if (r != ENTELEA_OK) {
		return r;
	}
	table->grant[cls][op] |= bitmap;
	return ENTELEA_OK;
}

entelea_result_t
entelea_perm_parse_list(const char *list, size_t len,
			entelea_perm_table_t *table)
{
	if ((list == NULL && len != 0) || table == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (len == 0) {
		return ENTELEA_OK;
	}
	size_t start = 0;

	for (;;) {
		char decl[64];
		size_t end = start;

		while (end < len && list[end] != ';') {
			end++;
		}
		const size_t n = end - start;

		if (n == 0 || n >= sizeof(decl)) {
			return ENTELEA_E_PARAM; /* Empty or overlong segment */
		}
		memcpy(decl, list + start, n);
		decl[n] = '\0';
		entelea_result_t r = entelea_perm_parse(decl, table);

		if (r != ENTELEA_OK) {
			return r;
		}
		if (end == len) {
			return ENTELEA_OK;
		}
		start = end + 1;
	}
}

entelea_result_t
entelea_principal_bind(entelea_principal_t *ctx, uint16_t app_id,
		       const entelea_perm_table_t *table)
{
	if (ctx == NULL || table == NULL) {
		return ENTELEA_E_PARAM;
	}
	ctx->_handle = 0;
	k_spinlock_key_t key = k_spin_lock(&contexts_lock);

	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (contexts[i].used && contexts[i].app_id == app_id) {
			k_spin_unlock(&contexts_lock, key);
			return ENTELEA_E_PARAM; /* app_id already bound */
		}
	}
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (!contexts[i].used && contexts[i].gen != UINT16_MAX) {
			contexts[i].used = true;
			contexts[i].app_id = app_id;
			contexts[i].table = *table;
			contexts[i].gen++;
			ctx->_handle = (uint8_t) (i + 1);
			ctx->_gen = contexts[i].gen;
			ctx->app_id = app_id;
			k_spin_unlock(&contexts_lock, key);
			return ENTELEA_OK;
		}
	}
	k_spin_unlock(&contexts_lock, key);
	return ENTELEA_E_NOMEM; /* Slots full or generations exhausted */
}

static struct bound_ctx *
resolve(entelea_principal_t ctx)
{
	/* Handle out of range, unbound, or app_id/generation mismatch means
	 * forged or stale: reject without leaking slot state. Slots whose
	 * generation is exhausted are never reused, avoiding wrap-around
	 * within this run. */
	if (ctx._handle == 0 || ctx._handle > MAX_BOUND_CTX) {
		return NULL;
	}
	struct bound_ctx *b = &contexts[ctx._handle - 1];

	if (!b->used || b->app_id != ctx.app_id || b->gen != ctx._gen) {
		return NULL;
	}
	return b;
}

void
entelea_principal_unbind(entelea_principal_t *ctx)
{
	if (ctx == NULL) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&contexts_lock);
	struct bound_ctx *b = resolve(*ctx);

	if (b != NULL) {
		b->used = false; /* Old handles now get E_PERM */
	}
	ctx->_handle = 0;
	k_spin_unlock(&contexts_lock, key);
}

static void
deny_trace(entelea_principal_t ctx, uint16_t detail)
{
	const struct {
		uint16_t app_id;
		uint16_t detail;
	} payload = { .app_id = ctx.app_id, .detail = detail };
	_Static_assert(sizeof(payload) <= sizeof(uint32_t),
		       "permission denial payload exceeds event copy capacity");
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_PERM_DENIED,
		.data = &payload,
		.len = sizeof(payload),
	};

	(void) entelea_evt_publish_copy(&evt); /* Bus counts drops when full */
}

entelea_result_t
entelea_perm_check(entelea_principal_t ctx, entelea_perm_class_t cls,
		   entelea_perm_op_t op, uint8_t inst)
{
	if ((int) cls < 0 || cls >= ENTELEA_PERM_CLASS_COUNT || (int) op < 0 ||
	    op >= ENTELEA_PERM_OP_COUNT || inst > 31) {
		return ENTELEA_E_PARAM;
	}
	k_spinlock_key_t key = k_spin_lock(&contexts_lock);
	struct bound_ctx *b = resolve(ctx);
	bool allowed = false;

	if (b != NULL) {
#ifdef ENTELEA_TEST
		if (resolve_hook != NULL) {
			resolve_hook();
		}
#endif
		allowed = (b->table.grant[cls][op] & (1U << inst)) != 0;
	}
	k_spin_unlock(&contexts_lock, key);
	if (!allowed) {
		deny_trace(ctx, (uint16_t) ((cls << 8) | op));
		return ENTELEA_E_PERM; /* Forged, stale, or undeclared */
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_perm_check_msg(entelea_principal_t ctx, const char *target)
{
	if (target == NULL) {
		return ENTELEA_E_PARAM;
	}
	size_t target_len = 0;

	while (target_len < ENTELEA_PERM_MSG_NAME_MAX &&
	       target[target_len] != '\0') {
		target_len++;
	}
	bool valid_target = valid_msg_name(target, target_len);
	k_spinlock_key_t key = k_spin_lock(&contexts_lock);
	struct bound_ctx *b = resolve(ctx);
	bool allowed = false;
	uint16_t detail = b == NULL ? 0xFFFF : 0xFFFE;

	if (b != NULL && valid_target) {
#ifdef ENTELEA_TEST
		if (resolve_hook != NULL) {
			resolve_hook();
		}
#endif
		for (int i = 0; i < ENTELEA_PERM_MSG_TARGETS_MAX; i++) {
			const char *name = b->table.msg_targets[i];

			if (name[target_len] == '\0' &&
			    memcmp(name, target, target_len) == 0) {
				allowed = true;
				break;
			}
		}
	}
	k_spin_unlock(&contexts_lock, key);
	if (!allowed) {
		deny_trace(ctx, detail);
		return ENTELEA_E_PERM;
	}
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void
entelea_perm_test_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&contexts_lock);

	memset(contexts, 0, sizeof(contexts));
	k_spin_unlock(&contexts_lock, key);
}
#endif
