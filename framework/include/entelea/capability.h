/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CAPABILITY_H
#define ENTELEA_CAPABILITY_H

/* R04 能力注册合同见 framework/execution/capability/contract.md。
 * 外设描述符（静态注册，回滚不留半注册）+ 按需能力检查。
 * DETACH 语义经 safety force_channel_fault（既有机制面）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>
#include <entelea/safety.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_PERIPH_DESCS_MAX 24 /* DEC-27 #7（对齐旧上限） */

typedef enum {
	ENTELEA_DEV_OUTPUT = 0, /* 输出通道（经保护层，必带 safety uid） */
	ENTELEA_DEV_INPUT,      /* 输入（不经保护层——合同 3） */
	ENTELEA_DEV_SENSOR,
	ENTELEA_DEV_KIND_COUNT,
} entelea_dev_kind_t;

typedef struct {
	const char *uid;          /* 逻辑名（注册期冻结） */
	entelea_dev_kind_t kind;
	uint8_t inst;             /* 权限实例号（≤31，与能力位图对齐） */
	entelea_perm_class_t perm_class;
	const char *safety_uid;   /* 输出通道的 safety 通道 uid（NULL=无） */
} entelea_periph_desc_t;

/** [boot] 批量注册（原子：任一非法/撞名/超限 → 全部回滚，E_PARAM）。 */
entelea_result_t entelea_periph_register(const entelea_periph_desc_t *descs,
					 size_t count);

/** [any] 按 uid 查描述符（NULL=未注册）。 */
const entelea_periph_desc_t *entelea_periph_find(const char *uid);

/** [any] 注册数。 */
size_t entelea_periph_count(void);

/** [any] 能力门 + 输出通道地址：检查 principal 对描述符的类/实例授权；
 * 输出设备额外要求其 safety 通道存在且处于可提交态。未经此门的
 * 直接 safety commit 属调用方违约（评审面）。 */
entelea_result_t entelea_periph_acquire(entelea_principal_t ctx,
					const char *uid,
					const entelea_periph_desc_t **out);

/** [boot/test] DETACH：物理不在场 → safety 单通道 fault（既有机制面），
 * 描述符保留（重附经 entelea_safety_channel_recover）。 */
entelea_result_t entelea_periph_detach(const char *uid);

#ifdef ENTELEA_TEST
void entelea_periph_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_CAPABILITY_H */
