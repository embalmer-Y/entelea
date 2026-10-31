/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tessera 公共错误码（design/LLD-ts-common.md §2——唯一出处，模块不得私造）。
 */
#ifndef TS_ERR_H__
#define TS_ERR_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t ts_res_t; /* TS_OK=0；负数为错误 */

#define TS_OK                 0
#define TS_E_PARAM           (-1)  /* 非法参数/描述符缺字段 */
#define TS_E_NOMEM           (-2)
#define TS_E_PERM            (-3)  /* 权限拒绝（合同10：留痕） */
#define TS_E_STATE           (-4)  /* 状态机/生命周期拒绝 */
#define TS_E_BUSY            (-5)
#define TS_E_TIMEOUT         (-6)
#define TS_E_NOTFOUND        (-7)  /* 通道/外设/APP 不存在 */
#define TS_E_IO              (-8)  /* 驱动/存储失败 */
#define TS_E_INVALID_SIG     (-9)  /* 包验签失败 */
#define TS_E_ROLLBACK_LIMIT (-10)
#define TS_E_UNINIT         (-11)
#define TS_E_RANGE          (-12)  /* 限幅/slew/预算拒绝（输出被安全层拦截） */
#define TS_E_PENDING        (-13)  /* 结构化中间态：激活待本地确认（F3 · DEC-62①——非失败） */

/*
 * 故障原因码（LLD-ts-common §2.1，DR-09）：u32 reason = 来源<<16 | 细因。
 * 用于 ts_safety_system_fail() 与 noinit 留痕；不得挪用为通用 API 返回值。
 */
#define TS_FAIL_SRC_BOOT    0x0001U /* 细因 = 失败的 boot 步骤 idx */
#define TS_FAIL_SRC_WDT     0x0002U /* 细因 = ts_wdt_src_t */
#define TS_FAIL_SRC_SAFETY  0x0003U /* 细因 = ts-safety 自定义 */
#define TS_FAIL_SRC_STORE   0x0004U /* 细因 = ts-store 错误细分 */

#define TS_FAIL_BOOT(idx)   ((uint32_t)TS_FAIL_SRC_BOOT << 16 | (uint32_t)(idx))
#define TS_FAIL_WDT(src)    ((uint32_t)TS_FAIL_SRC_WDT << 16 | (uint32_t)(src))

/* 调用者不透明上下文（LLD-ts-common §3.1，DR-10）：ts-appmgr 每 APP 实例化并注入；
 * M2 起 ts-hal/ts-power 以其为第一参数。防伪造边界：wasm 侧只见整数 id。 */
struct ts_ctx_opaque;
typedef struct ts_ctx_opaque ts_ctx_t;

#ifdef __cplusplus
}
#endif

#endif /* TS_ERR_H__ */
