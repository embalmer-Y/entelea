/* SPDX-License-Identifier: Apache-2.0 */
/* Linux host stub: the DT macro surface is empty (no board-level DT; all
 * DT branches in drivers.c are disabled, and only the sim backend takes
 * part). */
#ifndef ENTELEA_LINUX_DT_STUB
#define ENTELEA_LINUX_DT_STUB
#define DT_NODE_EXISTS(n) 0
#define DT_NODE_HAS_PROP(n, p) 0
#define DT_PATH(p) 0
#define DT_ALIAS(a) 0
#endif
