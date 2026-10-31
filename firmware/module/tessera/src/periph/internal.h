/* SPDX-License-Identifier: Apache-2.0 */
/* ts-periph 内部共享（不对外）。 */
#ifndef TS_PERIPH_INTERNAL_H__
#define TS_PERIPH_INTERNAL_H__

#include <stddef.h>
#include <ts/periph.h>

/* desc.c 供给 hotplug.c：按 uid 查描述符 */
const ts_periph_desc_t *periph_find(const char *uid);

#endif /* TS_PERIPH_INTERNAL_H__ */
