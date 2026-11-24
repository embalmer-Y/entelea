/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_LIBC_HEAP_H
#define ENTELEA_PLATFORM_LIBC_HEAP_H

#include <stdbool.h>
#include <stddef.h>

/* Number of memory pools declared for the C library's allocator in the
 * devicetree. Zero until they are registered (APPLICATION init level) and
 * on a board that declares none. */
size_t
entelea_libc_heap_pools(void);

/* Total size in bytes of the declared pools. */
size_t
entelea_libc_heap_bytes(void);

/* True when ptr lies inside a declared pool, i.e. it was handed out by the
 * C library's allocator after the pools were registered. */
bool
entelea_libc_heap_owns(const void *ptr);

#endif /* ENTELEA_PLATFORM_LIBC_HEAP_H */
