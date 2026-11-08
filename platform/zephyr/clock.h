/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_CLOCK_H
#define ENTELEA_PLATFORM_CLOCK_H

#include <entelea/time.h>

/* 初始化绑定到 Zephyr k_uptime 的实例时钟（ISR 安全原始读；见 clock.c）。 */
entelea_result_t entelea_platform_clock_init(entelea_clock_t *clock);

#endif /* ENTELEA_PLATFORM_CLOCK_H */
