/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PRINCIPAL_H
#define ENTELEA_PRINCIPAL_H

/* R04 可信调用者与能力裁决合同见 framework/execution/capability/contract.md。
 * principal 不透明（R01 合同）：仅可信宿主经 bind 构造；Wasm/Lua 输入
 * 整数不能转为 principal。能力文法 class:op:instances（数值位图）与
 * msg:name 名单（P1-1）；越权一律拒绝并留痕（EVT_PERM_DENIED）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ENTELEA_PERM_GPIO = 0,
	ENTELEA_PERM_PWM,
	ENTELEA_PERM_ADC,
	ENTELEA_PERM_POWER,
	ENTELEA_PERM_FS,
	ENTELEA_PERM_AV,
	ENTELEA_PERM_LOG,
	ENTELEA_PERM_KV,
	ENTELEA_PERM_MSG,
	ENTELEA_PERM_SYS,
	ENTELEA_PERM_CLASS_COUNT,
} entelea_perm_class_t;

typedef enum {
	ENTELEA_PERM_READ = 0,
	ENTELEA_PERM_WRITE,
	ENTELEA_PERM_SET,
	ENTELEA_PERM_LIST,
	ENTELEA_PERM_DELETE,
	ENTELEA_PERM_SEND,
	ENTELEA_PERM_OP_COUNT,
} entelea_perm_op_t;

#define ENTELEA_PERM_MSG_NAME_MAX 40
#define ENTELEA_PERM_MSG_TARGETS_MAX 4

typedef struct {
	uint32_t grant[ENTELEA_PERM_CLASS_COUNT][ENTELEA_PERM_OP_COUNT];
	char msg_targets[ENTELEA_PERM_MSG_TARGETS_MAX][ENTELEA_PERM_MSG_NAME_MAX];
} entelea_perm_table_t;

/* principal：不透明形态由 R01 context.h 前向声明（struct entelea_principal）；
 * 此处给出完整定义（同一定义源）。句柄=绑定槽索引+1（0=无效）；
 * 不可由外部整数构造（bind 是唯一入口，宿主可信面）。 */
#include <entelea/context.h>

struct entelea_principal {
	uint8_t _handle; /* 0=未绑定；1..MAX=槽+1 */
	uint16_t app_id;
};

/** 能力表初始化（全零=无任何授权）。 */
void entelea_perm_table_init(entelea_perm_table_t *table);

/** [host] 解析并合并一条能力声明（class:op:instances 位图或
 * msg:send:name）。域外类/操作/实例>31/文法坏 = E_PARAM（fail-closed，
 * 不静默截断）。重复声明幂等。 */
entelea_result_t entelea_perm_parse(const char *cap, entelea_perm_table_t *table);

/** [host] 绑定 principal（槽上限 4；app_id 撞名拒绝）。解绑后句柄失效
 * ——旧句柄的后续裁决一律 E_PERM（伪造/过期句柄不可绕）。 */
entelea_result_t entelea_principal_bind(entelea_principal_t *ctx,
					uint16_t app_id,
					const entelea_perm_table_t *table);

/** [host] 解绑（appmgr 停止链）。 */
void entelea_principal_unbind(entelea_principal_t *ctx);

/** [any] 裁决：未声明/未绑定/句柄伪造 = E_PERM + 留痕事件。 */
entelea_result_t entelea_perm_check(entelea_principal_t ctx,
				    entelea_perm_class_t cls,
				    entelea_perm_op_t op, uint8_t inst);

/** [any] msg 名单裁决（send 专用；recv 被动无权限门）。 */
entelea_result_t entelea_perm_check_msg(entelea_principal_t ctx,
					const char *target);

#ifdef ENTELEA_TEST
void entelea_perm_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PRINCIPAL_H */
