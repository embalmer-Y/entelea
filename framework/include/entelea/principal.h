/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PRINCIPAL_H
#define ENTELEA_PRINCIPAL_H

/* R04 可信调用者与能力裁决合同见 framework/execution/capability/contract.md。
 * 仅可信宿主可绑定和传递 principal；Wasm/Lua 输入整数不能转为 principal。
 * 公开 C 结构不是不可伪造的令牌，同地址空间依赖宿主信任边界。
 * 能力文法 class:op:instances（数值位图）与
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
	char msg_targets[ENTELEA_PERM_MSG_TARGETS_MAX]
			[ENTELEA_PERM_MSG_NAME_MAX];
} entelea_perm_table_t;

/* context.h 前向声明；此处公开完整定义，以值传递句柄。
 * bind 是可信宿主约定的构造入口，不是 C 语言级的不可伪造保证。 */
#include <entelea/context.h>

struct entelea_principal {
	uint8_t _handle; /* 0=未绑定；1..MAX=槽+1 */
	uint16_t _gen; /* 槽绑定代数（每次 bind 递增，非 0）——防 ABA */
	uint16_t app_id;
};

/** 能力表初始化（全零=无任何授权）。 */
void
entelea_perm_table_init(entelea_perm_table_t *table);

/** [host] 解析并合并一条能力声明（class:op:instances 位图或
 * msg:send:name）。域外类/操作/实例>31/文法坏 = E_PARAM（fail-closed，
 * 不静默截断）。重复声明幂等。 */
entelea_result_t
entelea_perm_parse(const char *cap, entelea_perm_table_t *table);

/** [host] 解析分号分隔的声明列表（空 = 无声明，OK）。任一段为空、超长或
 * 文法坏 = E_PARAM；失败后 table 内容未定义，调用方须丢弃。 */
entelea_result_t
entelea_perm_parse_list(const char *list, size_t len,
			entelea_perm_table_t *table);

/** [host] 绑定 principal（槽上限 4；app_id 撞名拒绝）。解绑后句柄失效
 * ——旧句柄（含宿主别处保存的副本）的后续裁决一律 E_PERM；句柄须与
 * 槽当前绑定的 app_id 与代数同时一致。槽代数耗尽后本次运行不再复用；
 * 无可用槽 = E_NOMEM。句柄只在当前运行有效，不得持久化或跨重启复用。 */
entelea_result_t
entelea_principal_bind(entelea_principal_t *ctx, uint16_t app_id,
		       const entelea_perm_table_t *table);

/** [host] 解绑（appmgr 停止链）。仅当 ctx 是槽当前绑定时生效——过期
 * 副本解绑为空操作（不得撤销同槽新绑定）。 */
void
entelea_principal_unbind(entelea_principal_t *ctx);

/** [any] 裁决：未声明/未绑定/句柄字段不匹配 = E_PERM + 尝试留痕事件。
 * 绑定、解绑和裁决同步；成功裁决不保证后续动作期间权限未被撤销。 */
entelea_result_t
entelea_perm_check(entelea_principal_t ctx, entelea_perm_class_t cls,
		   entelea_perm_op_t op, uint8_t inst);

/** [any] msg 名单裁决（send 专用；recv 被动无权限门）。
 * 名称须符合声明文法并在 40 字节内终止，否则 E_PERM；NULL 为 E_PARAM。 */
entelea_result_t
entelea_perm_check_msg(entelea_principal_t ctx, const char *target);

#ifdef ENTELEA_TEST
void
entelea_perm_test_reset(void);
void
entelea_perm_test_after_resolve(void (*hook)(void));
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PRINCIPAL_H */
