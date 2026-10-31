/* SPDX-License-Identifier: Apache-2.0 */
/* ts-interact 核心逻辑声明（组件内 + 测试消费；LLD-ts-interact §5）。
 * shell 命令表为编译期注册的打印薄壳，不经此头。 */
#ifndef TS_INTERACT_INTERNAL_H__
#define TS_INTERACT_INTERNAL_H__

#include <stdbool.h>
#include <ts/appmgr.h>
#include <ts/core.h>

/** 挂起安装确认（= BOOT 短按语义）：无 pending = TS_E_STATE。 */
ts_res_t ts_interact_confirm(ts_app_info_t *out);

/** 挂起安装拒绝（= BOOT 长按语义）：无 pending = TS_E_STATE。 */
ts_res_t ts_interact_reject(void);

/** 席位切换（CG3 同入口）：local=true → TS_ARB_LOCAL。 */
ts_res_t ts_interact_arb(bool local);

/** 本地故障清除（F2 同入口）：无 fault = TS_E_STATE。 */
ts_res_t ts_interact_estop_clear(void);

/** 全局日志详度（off/err/wrn/inf/dbg；域外 = TS_E_PARAM）。 */
ts_res_t ts_interact_log_level(const char *lvl);

#endif /* TS_INTERACT_INTERNAL_H__ */
