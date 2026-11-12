/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_SECURE_ZERO_H
#define ENTELEA_PLATFORM_SECURE_ZERO_H

#include <stddef.h>

static inline void
entelea_secure_zero(void *buffer, size_t size)
{
	volatile unsigned char *p = buffer;

	while (size-- > 0) {
		*p++ = 0;
	}
}

#endif /* ENTELEA_PLATFORM_SECURE_ZERO_H */
