// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <ts/hal.h>
#include <ts/periph.h>
#include <ts/power.h>
#include <ts/safety.h>
#include <zephyr/ztest.h>

ZTEST(periph_registration, test_failures_leave_no_partial_registration)
{
	static char uids[CONFIG_TS_HAL_MAX_INSTANCES][16];
	static const ts_periph_desc_t good = {
		.uid = "good",
		.kind = TS_PK_GPIO,
		.safe = {.uid = "good",
			 .kind = TS_CH_GPIO,
			 .poweron = {.b = false},
			 .linkloss = {.b = false},
			 .fault = {.b = false}},
	};
	static const ts_periph_desc_t gpio = {
		.uid = "rejected_gpio",
		.kind = TS_PK_GPIO,
		.safe = {.uid = "rejected_gpio",
			 .kind = TS_CH_GPIO,
			 .poweron = {.b = false},
			 .linkloss = {.b = false},
			 .fault = {.b = false}},
	};
	static const ts_periph_desc_t power = {
		.uid = "rejected_power",
		.kind = TS_PK_POWER,
		.safe = {.uid = "rejected_power",
			 .kind = TS_CH_POWER,
			 .poweron = {.pwr = {false, 0}},
			 .linkloss = {.pwr = {false, 0}},
			 .fault = {.pwr = {false, 0}},
			 .limits = {.current_limit_ma = 100}},
	};
	ts_periph_desc_t invalid = good;
	ts_safety_summary_t summary;
	ts_ch_state_t state;
	static const ts_ch_kind_t wrong_kind[] = {TS_CH_PWM, TS_CH_POWER, TS_CH_GPIO};

	ts_safety_test_reset();
	ts_periph_test_reset();
	ts_power_test_reset();
	for (int kind = TS_PK_GPIO; kind <= TS_PK_POWER; kind++) {
		invalid.kind = kind;
		invalid.safe.kind = wrong_kind[kind];
		zassert_equal(ts_periph_register(&invalid), TS_E_PARAM);
		zassert_equal(ts_periph_count(), 0);
		zassert_equal(ts_hal_dev_count(), 0);
		zassert_equal(ts_power_slot_count(), 0);
		zassert_equal(ts_safety_channel_state("good", &state), TS_E_NOTFOUND);
	}
	zassert_equal(ts_periph_register(&good), TS_OK);
	for (size_t i = ts_hal_dev_count(); i < CONFIG_TS_HAL_MAX_INSTANCES; i++) {
		snprintf(uids[i], sizeof(uids[i]), "filler%zu", i);
		const ts_hal_dev_desc_t dev = {.uid = uids[i], .kind = TS_DEV_ADC};

		zassert_equal(ts_hal_register_dev(&dev), TS_OK);
	}
	zassert_equal(ts_periph_register(&gpio), TS_E_NOMEM);
	zassert_equal(ts_periph_register(&power), TS_E_NOMEM);
	zassert_equal(ts_periph_count(), 1);
	zassert_equal(ts_hal_dev_count(), CONFIG_TS_HAL_MAX_INSTANCES);
	zassert_equal(ts_power_slot_count(), 0);
	zassert_equal(ts_safety_summary(&summary), TS_OK);
	zassert_equal(summary.channels, 1);
	zassert_equal(ts_safety_channel_state(gpio.uid, &state), TS_E_NOTFOUND);
	zassert_equal(ts_safety_channel_state(power.uid, &state), TS_E_NOTFOUND);
	zassert_equal(ts_safety_channel_state(good.uid, &state), TS_OK);
	zassert_equal(state, TS_ST_SAFE_POWERON);
	zassert_equal(ts_periph_get(0)->kind, TS_PK_GPIO);
}

ZTEST_SUITE(periph_registration, NULL, NULL, NULL, NULL, NULL);
