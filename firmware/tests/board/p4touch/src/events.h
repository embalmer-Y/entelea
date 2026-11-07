/* SPDX-License-Identifier: Apache-2.0 */
#ifndef P4_TOUCH_EVENTS_H_
#define P4_TOUCH_EVENTS_H_

#include <stdbool.h>
#include <stdint.h>

enum touch_result {
	TOUCH_NONE,
	TOUCH_DOWN,
	TOUCH_MOVE,
	TOUCH_UP,
};

struct touch_state {
	int32_t x;
	int32_t y;
	int32_t last_x;
	int32_t last_y;
	bool have_x;
	bool have_y;
	bool pressed;
};

int touch_event_apply(struct touch_state *state, uint8_t type, uint16_t code, int32_t value);

#endif
