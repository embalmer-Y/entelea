// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/ztest.h>

#include "events.h"

static void position(struct touch_state *state, int32_t x, int32_t y)
{
	zassert_equal(touch_event_apply(state, INPUT_EV_ABS, INPUT_ABS_X, x), TOUCH_NONE);
	zassert_equal(touch_event_apply(state, INPUT_EV_ABS, INPUT_ABS_Y, y), TOUCH_NONE);
}

ZTEST(touch_events, test_press_move_release)
{
	struct touch_state state = {0};

	position(&state, 0, 459);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), TOUCH_DOWN);
	position(&state, 0, 459);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), TOUCH_NONE);
	position(&state, 459, 0);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), TOUCH_MOVE);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 0), TOUCH_UP);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 0), TOUCH_NONE);
}

ZTEST(touch_events, test_out_of_bounds)
{
	struct touch_state state = {0};

	position(&state, 460, 0);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), -ERANGE);
	position(&state, 0, -1);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), -ERANGE);
	position(&state, 0, 460);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), -ERANGE);
}

ZTEST(touch_events, test_incomplete_frame_and_invalid_key)
{
	struct touch_state state = {0};

	zassert_equal(touch_event_apply(&state, INPUT_EV_ABS, INPUT_ABS_X, 10), TOUCH_NONE);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), -EPROTO);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 2), -EINVAL);
	zassert_equal(touch_event_apply(&state, INPUT_EV_ABS, INPUT_ABS_Y, 10), TOUCH_NONE);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 0), TOUCH_NONE);
	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, INPUT_BTN_TOUCH, 1), -EPROTO);
}

ZTEST(touch_events, test_unrelated_event)
{
	struct touch_state state = {0};

	zassert_equal(touch_event_apply(&state, INPUT_EV_KEY, 0, 1), TOUCH_NONE);
	zassert_equal(touch_event_apply(&state, INPUT_EV_ABS, 99, 1), TOUCH_NONE);
	zassert_false(state.pressed);
}

ZTEST_SUITE(touch_events, NULL, NULL, NULL, NULL, NULL);
