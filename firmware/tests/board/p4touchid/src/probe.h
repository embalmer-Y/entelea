/* SPDX-License-Identifier: Apache-2.0 */
#ifndef P4_TOUCH_ID_PROBE_H_
#define P4_TOUCH_ID_PROBE_H_

#include <stdint.h>
#include <zephyr/device.h>

int p4_touch_id_probe(const struct device *bus, uint8_t *id);

#endif
