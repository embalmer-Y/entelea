/* SPDX-License-Identifier: Apache-2.0 */
/* Linux 宿主桩：DT 宏面为空（无板级 DT——drivers.c 的 DT 分支全部
 * 关闭，仅 sim 后端参与）。 */
#ifndef ENTELEA_LINUX_DT_STUB
#define ENTELEA_LINUX_DT_STUB
#define DT_NODE_EXISTS(n) 0
#define DT_NODE_HAS_PROP(n, p) 0
#define DT_PATH(p) 0
#define DT_ALIAS(a) 0
#endif
