/* SPDX-License-Identifier: Apache-2.0 */
#ifndef P4W_NETWORK_H
#define P4W_NETWORK_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

int p4_uart_read_exact(const struct device *uart, uint8_t *buf, size_t size, k_timepoint_t end);
int p4_tls_test(void);

#endif
