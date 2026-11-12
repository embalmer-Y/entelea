/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TEST_ZEPHYR_KERNEL_H
#define TEST_ZEPHYR_KERNEL_H
#include <stdint.h>
#include <stdio.h>
int64_t k_uptime_get(void);
#define printk printf
#define snprintk snprintf
#endif
