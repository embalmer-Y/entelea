// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include "events.h"

#define TOUCH_WIDTH 460
#define TOUCH_HEIGHT 460

int touch_event_apply(struct touch_state *state, uint8_t type, uint16_t code, int32_t value)
{
	int result;

	if (type == INPUT_EV_ABS && code == INPUT_ABS_X) {
		state->x = value;
		state->have_x = true;
		return TOUCH_NONE;
	}
	if (type == INPUT_EV_ABS && code == INPUT_ABS_Y) {
		state->y = value;
		state->have_y = true;
		return TOUCH_NONE;
	}
	if (type != INPUT_EV_KEY || code != INPUT_BTN_TOUCH) {
		return TOUCH_NONE;
	}
	if (value != 0 && value != 1) {
		return -EINVAL;
	}
	if (!value) {
		state->have_x = false;
		state->have_y = false;
		result = state->pressed ? TOUCH_UP : TOUCH_NONE;
		state->pressed = false;
		return result;
	}
	if (!state->have_x || !state->have_y) {
		return -EPROTO;
	}
	state->have_x = false;
	state->have_y = false;
	if (state->x < 0 || state->x >= TOUCH_WIDTH || state->y < 0 || state->y >= TOUCH_HEIGHT) {
		return -ERANGE;
	}
	result = !state->pressed					    ? TOUCH_DOWN
		 : (state->x != state->last_x || state->y != state->last_y) ? TOUCH_MOVE
									    : TOUCH_NONE;
	state->pressed = true;
	state->last_x = state->x;
	state->last_y = state->y;
	return result;
}
