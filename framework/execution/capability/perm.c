/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 能力文法解析 + 调用者裁决（迁自旧 hal/perm.c 的裁决面，R04 合同
 * framework/execution/capability/contract.md）。
 * principal 绑定槽 = 宿主可信面（原生侧映射，APP 不可寻址）；
 * 越权/伪造/过期句柄一律拒绝并留痕（EVT_PERM_DENIED，观测面）。
 * 相对旧实现：类/操作集裁至 R04 消费面（gpio/pwm/power/msg 为主，
 * 其余类仅登记文法——无消费者的 op 不放行新语义，fail-closed 保持）。
 */
#include <ctype.h>
#include <string.h>
#include <entelea/events.h>
#include <entelea/principal.h>

#define MAX_BOUND_CTX 4 /* 同时加载 APP 上限（DEC-27 #10） */

struct bound_ctx {
	uint16_t app_id;
	entelea_perm_table_t table;
	bool used;
};
static struct bound_ctx contexts[MAX_BOUND_CTX];

void entelea_perm_table_init(entelea_perm_table_t *table)
{
	if (table != NULL) {
		memset(table, 0, sizeof(*table));
	}
}

static int parse_class(const char *s, size_t len)
{
	if (len == 4 && strncmp(s, "gpio", 4) == 0) return ENTELEA_PERM_GPIO;
	if (len == 3 && strncmp(s, "pwm", 3) == 0) return ENTELEA_PERM_PWM;
	if (len == 3 && strncmp(s, "adc", 3) == 0) return ENTELEA_PERM_ADC;
	if (len == 5 && strncmp(s, "power", 5) == 0) return ENTELEA_PERM_POWER;
	if (len == 2 && strncmp(s, "fs", 2) == 0) return ENTELEA_PERM_FS;
	if (len == 2 && strncmp(s, "av", 2) == 0) return ENTELEA_PERM_AV;
	if (len == 3 && strncmp(s, "log", 3) == 0) return ENTELEA_PERM_LOG;
	if (len == 2 && strncmp(s, "kv", 2) == 0) return ENTELEA_PERM_KV;
	if (len == 3 && strncmp(s, "msg", 3) == 0) return ENTELEA_PERM_MSG;
	if (len == 3 && strncmp(s, "sys", 3) == 0) return ENTELEA_PERM_SYS;
	return -1;
}

static int parse_op(const char *s, size_t len)
{
	if (len == 4 && strncmp(s, "read", 4) == 0) return ENTELEA_PERM_READ;
	if (len == 5 && strncmp(s, "write", 5) == 0) return ENTELEA_PERM_WRITE;
	if (len == 3 && strncmp(s, "set", 3) == 0) return ENTELEA_PERM_SET;
	if (len == 4 && strncmp(s, "list", 4) == 0) return ENTELEA_PERM_LIST;
	if (len == 6 && strncmp(s, "delete", 6) == 0) return ENTELEA_PERM_DELETE;
	if (len == 4 && strncmp(s, "send", 4) == 0) return ENTELEA_PERM_SEND;
	return -1;
}

/* msg 名单文法：[A-Za-z0-9._-]{1,39}（zenoh key 段同源白名单）。 */
static bool valid_msg_name(const char *s, size_t len)
{
	if (len == 0 || len >= ENTELEA_PERM_MSG_NAME_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';

		if (!ok) {
			return false;
		}
	}
	return true;
}

static entelea_result_t msg_target_add(entelea_perm_table_t *table,
				       const char *s, size_t len)
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
			return ENTELEA_OK; /* 重复声明幂等 */
		}
	}
	return ENTELEA_E_NOMEM; /* 名单上限（装载期拒绝，不静默截断） */
}

/* 实例段数值位图："0-3" / "0,2" / "5"；实例>31 拒。 */
static entelea_result_t parse_instances(const char *s, size_t len, uint32_t *bitmap)
{
	size_t i = 0;

	while (i < len) {
		if (s[i] == ',') {
			i++;
			continue;
		}
		if (!isdigit((int)s[i])) {
			return ENTELEA_E_PARAM;
		}
		uint32_t start = 0, end = 0;

		while (i < len && isdigit((int)s[i])) {
			start = start * 10 + (uint32_t)(s[i] - '0');
			if (start > 31) {
				return ENTELEA_E_PARAM;
			}
			i++;
		}
		end = start;
		if (i < len && s[i] == '-') {
			i++;
			if (i >= len || !isdigit((int)s[i])) {
				return ENTELEA_E_PARAM;
			}
			end = 0;
			while (i < len && isdigit((int)s[i])) {
				end = end * 10 + (uint32_t)(s[i] - '0');
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

entelea_result_t entelea_perm_parse(const char *cap, entelea_perm_table_t *table)
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
	int cls = parse_class(cap, (size_t)(c1 - cap));

	if (cls < 0) {
		return ENTELEA_E_PARAM;
	}
	int op = parse_op(c1 + 1, (size_t)(c2 - c1 - 1));

	if (op < 0) {
		return ENTELEA_E_PARAM;
	}
	/* msg 类 = 名单形态（op 仅 send；recv 被动无权限门——其余 op 无
	 * 消费者，fail-closed）。 */
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

entelea_result_t entelea_principal_bind(entelea_principal_t *ctx, uint16_t app_id,
					const entelea_perm_table_t *table)
{
	if (ctx == NULL || table == NULL) {
		return ENTELEA_E_PARAM;
	}
	ctx->_handle = 0;
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (contexts[i].used && contexts[i].app_id == app_id) {
			return ENTELEA_E_PARAM; /* app_id 撞名拒绝 */
		}
	}
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (!contexts[i].used) {
			contexts[i].used = true;
			contexts[i].app_id = app_id;
			contexts[i].table = *table;
			ctx->_handle = (uint8_t)(i + 1);
			ctx->app_id = app_id;
			return ENTELEA_OK;
		}
	}
	return ENTELEA_E_NOMEM; /* 槽满（=APP 上限） */
}

void entelea_principal_unbind(entelea_principal_t *ctx)
{
	if (ctx == NULL || ctx->_handle == 0 || ctx->_handle > MAX_BOUND_CTX) {
		return;
	}
	struct bound_ctx *b = &contexts[ctx->_handle - 1];

	b->used = false; /* 旧句柄此后裁决 E_PERM（伪造/过期不可绕） */
	ctx->_handle = 0;
}

static struct bound_ctx *resolve(entelea_principal_t ctx)
{
	/* 句柄域外/未绑定 = 伪造 → 拒（不泄露槽状态）。 */
	if (ctx._handle == 0 || ctx._handle > MAX_BOUND_CTX) {
		return NULL;
	}
	struct bound_ctx *b = &contexts[ctx._handle - 1];

	return b->used ? b : NULL;
}

static void deny_trace(entelea_principal_t ctx, uint16_t detail)
{
	const struct {
		uint16_t app_id;
		uint16_t detail;
	} payload = {.app_id = ctx.app_id, .detail = detail};
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_PERM_DENIED,
		.data = &payload,
		.len = sizeof(payload),
	};

	entelea_evt_publish(&evt);
}

entelea_result_t entelea_perm_check(entelea_principal_t ctx,
				    entelea_perm_class_t cls,
				    entelea_perm_op_t op, uint8_t inst)
{
	if ((int)cls < 0 || cls >= ENTELEA_PERM_CLASS_COUNT ||
	    (int)op < 0 || op >= ENTELEA_PERM_OP_COUNT || inst > 31) {
		return ENTELEA_E_PARAM;
	}
	struct bound_ctx *b = resolve(ctx);

	if (b == NULL) {
		deny_trace(ctx, (uint16_t)((cls << 8) | op));
		return ENTELEA_E_PERM; /* 伪造/过期句柄 */
	}
	if ((b->table.grant[cls][op] & (1U << inst)) == 0) {
		deny_trace(ctx, (uint16_t)((cls << 8) | op));
		return ENTELEA_E_PERM; /* 未声明（合同 10） */
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_perm_check_msg(entelea_principal_t ctx, const char *target)
{
	if (target == NULL) {
		return ENTELEA_E_PARAM;
	}
	struct bound_ctx *b = resolve(ctx);

	if (b == NULL) {
		deny_trace(ctx, 0xFFFF);
		return ENTELEA_E_PERM;
	}
	for (int i = 0; i < ENTELEA_PERM_MSG_TARGETS_MAX; i++) {
		if (b->table.msg_targets[i][0] != '\0' &&
		    strcmp(b->table.msg_targets[i], target) == 0) {
			return ENTELEA_OK;
		}
	}
	deny_trace(ctx, 0xFFFE);
	return ENTELEA_E_PERM;
}

#ifdef ENTELEA_TEST
void entelea_perm_test_reset(void)
{
	memset(contexts, 0, sizeof(contexts));
}
#endif
