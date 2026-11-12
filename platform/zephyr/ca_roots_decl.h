/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CA_ROOTS_DECL_H
#define ENTELEA_CA_ROOTS_DECL_H

/* Embedded trust roots from ca_roots.c, consumed by the on-board https_chat. */
#include <stddef.h>

extern const char *const entelea_ca_roots[];
extern const size_t entelea_ca_root_count;

#endif
