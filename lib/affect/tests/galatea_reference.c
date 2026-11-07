// SPDX-License-Identifier: Apache-2.0
#include <stddef.h>

#include "galatea_reference.h"
#include "kaleido_emotion.h"
#include "suites.h"
#include "unity.h"

#define REFERENCE_TOLERANCE 0.000001f
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static void test_galatea_lowpass(void)
{
	unsigned int step;
	size_t i;
	float state;

	for (i = 0; i < ARRAY_SIZE(galatea_lowpass_cases); i++) {
		const struct galatea_lowpass_case *ref = &galatea_lowpass_cases[i];

		state = ref->initial;
		for (step = 0; step < ref->steps; step++) {
			state = ke_lowpass(state, ref->input, ref->tau);
		}
		TEST_ASSERT_FLOAT_WITHIN(REFERENCE_TOLERANCE, ref->expected, state);
	}
}

static void test_galatea_hysteresis(void)
{
	size_t i;
	bool result;

	for (i = 0; i < ARRAY_SIZE(galatea_hysteresis_cases); i++) {
		const struct galatea_hysteresis_case *ref = &galatea_hysteresis_cases[i];

		result = ke_hysteresis(ref->previous, ref->input, ref->band);
		TEST_ASSERT_EQUAL(ref->expected, result);
	}
}

static void test_galatea_valence_trajectories(void)
{
	struct ke_inputs in = {.have_drive = true};
	struct ke_output out;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(galatea_valence_cases); i++) {
		const struct galatea_valence_case *ref = &galatea_valence_cases[i];

		if (ref->reset) {
			ke_reset(NULL);
		}
		in.drive_error_sum = ref->error;
		ke_step(i, &in, &out);
		TEST_ASSERT_FLOAT_WITHIN(REFERENCE_TOLERANCE, ref->expected, out.state.valence);
	}
}

void galatea_reference_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_galatea_lowpass);
	RUN_TEST(test_galatea_hysteresis);
	RUN_TEST(test_galatea_valence_trajectories);
	UNITY_END();
}
