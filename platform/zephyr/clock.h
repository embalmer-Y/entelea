/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_CLOCK_H
#define ENTELEA_PLATFORM_CLOCK_H

#include <entelea/time.h>

/* Initialize the instance clock bound to Zephyr k_uptime (ISR-safe raw
 * read; see clock.c). */
entelea_result_t
entelea_platform_clock_init(entelea_clock_t *clock);

#endif /* ENTELEA_PLATFORM_CLOCK_H */
