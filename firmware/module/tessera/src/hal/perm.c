/* SPDX-License-Identifier: Apache-2.0 */
/* 能力文法解析 + 调用者裁决（LLD-ts-hal §2/§3）。
 * 合同 10：越权访问一律拒绝并留痕（TS_EVT_PERM_DENIED）。 */
#include <ctype.h>
#include <string.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include <zephyr/kernel.h>

/* ctx → perm 表绑定（一对一；原生侧映射，APP 不可寻址） */
#define MAX_BOUND_CTX 4 /* = 同时加载 APP 上限（DEC-27 #10） */

struct bound_ctx {
	uint16_t app_id;
	ts_perm_table_t table;
	bool used;
};
static struct bound_ctx contexts[MAX_BOUND_CTX];

void ts_perm_table_init(ts_perm_table_t *table)
{
	memset(table, 0, sizeof(*table));
}

static int parse_class(const char *s, size_t len)
{
	if (len == 4 && strncmp(s, "gpio", 4) == 0) return TS_PERM_CLASS_GPIO;
	if (len == 3 && strncmp(s, "pwm", 3) == 0) return TS_PERM_CLASS_PWM;
	if (len == 3 && strncmp(s, "adc", 3) == 0) return TS_PERM_CLASS_ADC;
	if (len == 5 && strncmp(s, "power", 5) == 0) return TS_PERM_CLASS_POWER;
	if (len == 2 && strncmp(s, "fs", 2) == 0) return TS_PERM_CLASS_FS; /* DEC-47⑤ */
	if (len == 2 && strncmp(s, "av", 2) == 0) return TS_PERM_CLASS_AV; /* DEC-47⑤ */
	if (len == 3 && strncmp(s, "log", 3) == 0) return TS_PERM_CLASS_LOG; /* V0.4.0 M2 */
	if (len == 2 && strncmp(s, "kv", 2) == 0) return TS_PERM_CLASS_KV; /* V0.4.0 M9 */
	if (len == 3 && strncmp(s, "msg", 3) == 0) return TS_PERM_CLASS_MSG;
	if (len == 3 && strncmp(s, "sys", 3) == 0) return TS_PERM_CLASS_SYS;
	return -1;
}

static int parse_op(const char *s, size_t len)
{
	if (len == 4 && strncmp(s, "read", 4) == 0) return TS_PERM_OP_READ;
	if (len == 5 && strncmp(s, "write", 5) == 0) return TS_PERM_OP_WRITE;
	if (len == 3 && strncmp(s, "set", 3) == 0) return TS_PERM_OP_SET;
	if (len == 4 && strncmp(s, "list", 4) == 0) return TS_PERM_OP_LIST; /* DEC-47⑤ */
	if (len == 6 && strncmp(s, "delete", 6) == 0) return TS_PERM_OP_DELETE; /* DEC-47⑤ */
	if (len == 4 && strncmp(s, "send", 4) == 0) return TS_PERM_OP_SEND; /* P1-1 */
	return -1;
}

/* P1-1（HLD §3.4）：msg 类目标名单形态——实例段为 APP 串身份
 *（[A-Za-z0-9._-]{1,39}，同 zenoh key 段白名单文法——IR2-11 同源）。
 * 数值形态对 msg 无语义 = 拒（fail-closed，IR-11 登记的实现面收口）。 */
static bool valid_msg_name(const char *s, size_t len)
{
	if (len == 0 || len >= TS_PERM_MSG_NAME_MAX) {
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

static ts_res_t msg_target_add(ts_perm_table_t *table, const char *s, size_t len)
{
	char name[TS_PERM_MSG_NAME_MAX];

	memcpy(name, s, len);
	name[len] = '\0';
	for (int i = 0; i < TS_PERM_MSG_TARGETS_MAX; i++) {
		if (table->msg_targets[i][0] == '\0') {
			strcpy(table->msg_targets[i], name);
			return TS_OK;
		}
		if (strcmp(table->msg_targets[i], name) == 0) {
			return TS_OK; /* 重复声明幂等 */
		}
	}
	return TS_E_NOMEM; /* 名单上限 4（超 = 装载期拒绝，不静默截断） */
}

/* 解析实例段（数值文法）："0-3" / "0,2" / "5"。
 * V1 只收数值实例：msg 类 name 形态（LLD-ts-hal §2）未实现前一律 TS_E_PARAM
 * 拒绝（fail-closed，impl-review IR-11）——不允许声明未实现的能力形态。 */
static ts_res_t parse_instances(const char *s, size_t len, uint32_t *bitmap)
{
	size_t i = 0;

	while (i < len) {
		/* 跳过逗号 */
		if (s[i] == ',') {
			i++;
			continue;
		}
		if (!isdigit((int)s[i])) {
			return TS_E_PARAM;
		}
		uint32_t start = 0, end = 0;

		while (i < len && isdigit((int)s[i])) {
			start = start * 10 + (uint32_t)(s[i] - '0');
			if (start > 31) return TS_E_PARAM;
			i++;
		}
		end = start;
		if (i < len && s[i] == '-') {
			i++;
			if (i >= len || !isdigit((int)s[i])) return TS_E_PARAM;
			end = 0; /* 范围尾段独立解析（不从 start 累积） */
			while (i < len && isdigit((int)s[i])) {
				end = end * 10 + (uint32_t)(s[i] - '0');
				if (end > 31) return TS_E_PARAM;
				i++;
			}
		}
		if (end < start) return TS_E_PARAM;
		for (uint32_t b = start; b <= end; b++) {
			*bitmap |= (1U << b);
		}
	}
	return TS_OK;
}

ts_res_t ts_perm_parse(const char *cap, ts_perm_table_t *table)
{
	if (cap == NULL || table == NULL) {
		return TS_E_PARAM;
	}
	/* 格式：class:op:instances */
	const char *c1 = strchr(cap, ':');

	if (c1 == NULL) return TS_E_PARAM;
	const char *c2 = strchr(c1 + 1, ':');

	if (c2 == NULL) return TS_E_PARAM;

	int cls = parse_class(cap, (size_t)(c1 - cap));

	if (cls < 0) return TS_E_PARAM;
	int op = parse_op(c1 + 1, (size_t)(c2 - c1 - 1));

	if (op < 0) return TS_E_PARAM;

	/* P1-1：msg 类 = 名单形态（实例段为 APP 串身份，非数值位图）；
	 * op 仅 send（recv 被动无权限门——其余 op 无消费者，fail-closed） */
	if (cls == TS_PERM_CLASS_MSG) {
		size_t tlen = strlen(c2 + 1);

		if (op != TS_PERM_OP_SEND ||
		    !valid_msg_name(c2 + 1, tlen)) {
			return TS_E_PARAM;
		}
		ts_res_t mr = msg_target_add(table, c2 + 1, tlen);

		if (mr != TS_OK) {
			return mr;
		}
		/* op 开关位图同步置位（观测面一致：位图位 + 名单双记录） */
		table->bitmap[cls][op] |= 1U;
		return TS_OK;
	}

	uint32_t bitmap = 0;
	ts_res_t r = parse_instances(c2 + 1, strlen(c2 + 1), &bitmap);

	if (r != TS_OK) return r;
	table->bitmap[cls][op] |= bitmap;
	return TS_OK;
}

ts_res_t ts_perm_check(ts_ctx_t ctx, ts_perm_class_t cls, ts_perm_op_t op, uint8_t inst)
{
	/* 查绑定表 */
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (contexts[i].used && contexts[i].app_id == ctx.app_id) {
			if (cls < TS_PERM_CLASS_COUNT && op < TS_PERM_OP_COUNT && inst < 32) {
				if (contexts[i].table.bitmap[cls][op] & (1U << inst)) {
					return TS_OK;
				}
			}
			/* 合同 10：越权拒绝并留痕 */
			struct {
				uint16_t app_id;
				uint8_t cls, op, inst;
			} payload = {.app_id = ctx.app_id, .cls = (uint8_t)cls,
				     .op = (uint8_t)op, .inst = inst};
			const ts_evt_t evt = {
				.id = TS_EVT_PERM_DENIED,
				.t_ms = ts_time_ms(),
				.data = &payload,
				.len = sizeof(payload),
			};
			ts_evt_publish(&evt);
			return TS_E_PERM;
		}
	}
	return TS_E_PERM; /* 未绑定上下文 = 拒绝 */
}

ts_res_t ts_perm_check_msg(ts_ctx_t ctx, const char *target)
{
	if (target == NULL) {
		return TS_E_PARAM;
	}
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (contexts[i].used && contexts[i].app_id == ctx.app_id) {
			for (int t = 0; t < TS_PERM_MSG_TARGETS_MAX; t++) {
				if (contexts[i].table.msg_targets[t][0] != '\0' &&
				    strcmp(contexts[i].table.msg_targets[t], target) == 0) {
					return TS_OK;
				}
			}
			/* 合同 10：越权拒绝并留痕（与数值面同事件形态） */
			struct {
				uint16_t app_id;
				uint8_t cls, op, inst;
			} payload = {.app_id = ctx.app_id, .cls = TS_PERM_CLASS_MSG,
				     .op = TS_PERM_OP_SEND, .inst = 0};
			const ts_evt_t evt = {
				.id = TS_EVT_PERM_DENIED,
				.t_ms = ts_time_ms(),
				.data = &payload,
				.len = sizeof(payload),
			};
			ts_evt_publish(&evt);
			return TS_E_PERM;
		}
	}
	return TS_E_PERM; /* 未绑定上下文 = 拒 */
}

ts_res_t ts_hal_bind_context(ts_ctx_t *ctx, uint16_t app_id, const ts_perm_table_t *table)
{
	if (ctx == NULL || table == NULL) return TS_E_PARAM;
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (!contexts[i].used) {
			contexts[i].used = true;
			contexts[i].app_id = app_id;
			contexts[i].table = *table;
			ctx->app_id = app_id;
			ctx->_rsv = 0;
			return TS_OK;
		}
	}
	return TS_E_NOMEM; /* 同时加载上限（DEC-27 #10：4） */
}

void ts_hal_unbind_context(ts_ctx_t *ctx)
{
	if (ctx == NULL) return;
	for (int i = 0; i < MAX_BOUND_CTX; i++) {
		if (contexts[i].used && contexts[i].app_id == ctx->app_id) {
			contexts[i].used = false;
			return;
		}
	}
}
