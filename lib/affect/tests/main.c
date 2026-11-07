// SPDX-License-Identifier: Apache-2.0
#include "unity_internals.h"
#include "suites.h"

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
	unsigned int failures = 0;

	drive_tests_main();
	failures += Unity.TestFailures;
	emotion_tests_main();
	failures += Unity.TestFailures;
	ignition_tests_main();
	failures += Unity.TestFailures;
	ignition_w3_tests_main();
	failures += Unity.TestFailures;
	galatea_reference_tests_main();
	failures += Unity.TestFailures;

	return failures != 0;
}
