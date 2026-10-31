/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ts-periph 公共 API（design/LLD-ts-periph.md）：外设描述符管理与插拔事件、
 * 逻辑名→硬件资源绑定（可插拔外设框架侧落点；DEC-14 板差异隔离于此层）。
 * 合同关联：合同 1/10（描述符携带三安全态与安全参数，注册期冻结）。
 */
#ifndef TS_PERIPH_H__
#define TS_PERIPH_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/err.h>
#include <ts/safety.h> /* ts_out_ch_t（输出类描述符的安全声明） */

#ifdef __cplusplus
extern "C" {
#endif

/* DR-13：与 ts_ch_kind_t（仅输出通道）分离；ADC 只注册 ts-hal 输入侧 */
typedef enum {
	TS_PK_GPIO,
	TS_PK_PWM,
	TS_PK_POWER,
	TS_PK_ADC,
} ts_periph_kind_t;

typedef struct {
	const char *uid;     /* 逻辑名："gpio0"/"pwm1"/"pwr0"…（跨板稳定，编址用） */
	ts_periph_kind_t kind;
	const void *dt_spec; /* Zephyr devicetree spec（板相关，构建期绑定；桩 = NULL） */
	/* 输出类（GPIO/PWM/POWER）的安全声明：三安全态 + limits 随描述符冻结
	 * （合同 10）。safe.uid 必须等于 uid（单源）；ADC 忽略此域。
	 * POWER 类经 ts-power 槽注册（linkloss/fault 由 power 模块生成）。 */
	ts_out_ch_t safe;
} ts_periph_desc_t;

/** 注册职责链（LLD §2）：ts-periph（描述）→ ts-safety（输出通道，POWER 经
 * ts-power 槽）→ ts-hal（实例寻址）。撞 uid/三态缺失/safe.uid 不一致 →
 * TS_E_PARAM（注册即失败，不留半注册）。生命周期契约（F-2 修复后）：
 * 描述符**结构体**被拷入模块静态表（允许调用方栈上组装结构体本身）；
 * 其**字符串字段**（uid/dt_spec/safe.uid 指向的内容）须指向静态存储——
 * 注册表与 ts-safety 持这些指针的引用。
 * 运行期注册接口保留给热插拔（V1 仅测试用）。 */
ts_res_t ts_periph_register(const ts_periph_desc_t *d);

size_t ts_periph_count(void);
const ts_periph_desc_t *ts_periph_get(size_t idx);

/* ---- 插拔（hotplug.c，V1：事件 + 通道态；LLD §3）------------------------- */

/** 拔出：通道进 SAFE_FAULT（物理不在场 = 故障态，输出拒绝）+
 * TS_EVT_PERIPH_DETACH（key 下线通告经事件外发）。 */
ts_res_t ts_periph_detach(const char *uid);

/** 插入：TS_EVT_PERIPH_ATTACH + 通道恢复（上电态重放；estop 锁存期拒绝）。 */
ts_res_t ts_periph_attach(const char *uid);

#ifdef CONFIG_TS_TEST
void ts_periph_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TS_PERIPH_H__ */
