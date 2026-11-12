/* SPDX-License-Identifier: Apache-2.0 */
/* Synthetic, offline tests of the exact MCU P4C1/P4C2 credential parsers. */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../zephyr/apps/hw_cloud_chat/p4c2.h"

struct input_stream {
	const uint8_t *data;
	size_t size;
	size_t pos;
};

static int
read_input(void *user, uint8_t *dst, size_t size)
{
	struct input_stream *input = user;

	if (size > input->size - input->pos) {
		return -ETIMEDOUT;
	}
	memcpy(dst, input->data + input->pos, size);
	input->pos += size;
	return 0;
}

static void
pinned_driver_password_copy(const uint8_t *psk, char *out)
{
	memset(out, 0, 65);
	strncpy(out, (const char *) psk, 64);
}

int
main(void)
{
	uint8_t frame[9 + 32 + 63 + 63 + 127 + 47] = { 'P', '4', 'C', '2', 32,
						       63,  63,	 127, 47 };
	char driver_psk[65];
	int failures = 0;
	struct {
		struct gate_credentials value;
		uint8_t canary;
	} result;
	struct input_stream input = { frame, sizeof(frame), 0 };

	memset(frame + 9, 'S', 32);
	memset(frame + 9 + 32, 'P', 63);
	memset(frame + 9 + 32 + 63, 'H', 63);
	memset(frame + 9 + 32 + 63 + 63, 'K', 127);
	memset(frame + 9 + 32 + 63 + 63 + 127, 'M', 47);
	memset(&result, 0xa5, sizeof(result));
	if (entelea_p4c2_read(&result.value, read_input, &input) != 0 ||
	    input.pos != sizeof(frame) || result.value.wifi.ssid_length != 32 ||
	    result.value.wifi.psk_length != 63 ||
	    memcmp(result.value.wifi.ssid, frame + 9, 32) != 0 ||
	    memcmp(result.value.wifi.psk, frame + 9 + 32, 63) != 0 ||
	    result.value.wifi.ssid[32] != '\0' ||
	    result.value.wifi.psk[63] != '\0' ||
	    result.value.host[63] != '\0' ||
	    result.value.api_key[127] != '\0' ||
	    result.value.model[47] != '\0' || result.canary != 0xa5) {
		fprintf(stderr, "FAIL: maximum P4C2 field lengths\n");
		return 1;
	}
	pinned_driver_password_copy(result.value.wifi.psk, driver_psk);
	if (memcmp(driver_psk, frame + 9 + 32, 63) != 0 ||
	    driver_psk[63] != '\0') {
		fprintf(stderr, "FAIL: pinned driver must receive the exact "
				"63-byte PSK\n");
		failures++;
	}
	input.pos = 0;
	frame[9 + 5] = '\0';
	if (entelea_p4c2_read(&result.value, read_input, &input) != -EINVAL) {
		fprintf(stderr, "FAIL: SSID NUL cannot be sent by pinned "
				"string driver\n");
		failures++;
	}
	frame[9 + 5] = 'S';
	memset(&result, 0xa5, sizeof(result));
	input.size = 9 + 32;
	input.pos = 0;
	if (entelea_p4c2_read(&result.value, read_input, &input) !=
		    -ETIMEDOUT ||
	    result.value.wifi.psk[0] != 0xa5 ||
	    result.value.wifi.ssid_length != 0 ||
	    result.value.wifi.psk_length != 0 || result.canary != 0xa5) {
		fprintf(stderr, "FAIL: 32-byte SSID or partial PSK boundary\n");
		return 1;
	}
	input.size = sizeof(frame);
	input.pos = 0;
	frame[9 + 32 + 63 + 10] = '\0';
	memset(&result, 0xa5, sizeof(result));
	if (entelea_p4c2_read(&result.value, read_input, &input) != -EINVAL ||
	    (unsigned char) result.value.api_key[0] != 0xa5 ||
	    result.canary != 0xa5) {
		fprintf(stderr,
			"FAIL: embedded NUL must reject before API key\n");
		return 1;
	}
	input.pos = 0;
	frame[4] = 33;
	if (entelea_p4c2_read(&result.value, read_input, &input) != -EINVAL ||
	    input.pos != 9) {
		fprintf(stderr, "FAIL: invalid SSID length must reject\n");
		return 1;
	}
	input.pos = 0;
	frame[4] = 32;
	frame[7] = 19;
	if (entelea_p4c2_read(&result.value, read_input, &input) != -EINVAL ||
	    input.pos != 9) {
		fprintf(stderr, "FAIL: invalid API key length must reject\n");
		return 1;
	}
	{
		uint8_t p4c1[6 + 32 + 63] = { 'P', '4', 'C', '1', 32, 63 };
		struct entelea_wifi_credentials wifi;
		struct input_stream wifi_input = { p4c1, sizeof(p4c1), 0 };

		memset(p4c1 + 6, 'S', 32);
		memset(p4c1 + 6 + 32, 'P', 63);
		memset(&wifi, 0xa5, sizeof(wifi));
		if (entelea_wifi_read_p4c1(&wifi, read_input, &wifi_input) !=
			    0 ||
		    wifi.ssid_length != 32 || wifi.psk_length != 63 ||
		    wifi.ssid[32] != '\0' || wifi.psk[63] != '\0') {
			fprintf(stderr, "FAIL: maximum P4C1 fields\n");
			failures++;
		}
		pinned_driver_password_copy(wifi.psk, driver_psk);
		if (memcmp(driver_psk, p4c1 + 6 + 32, 63) != 0 ||
		    driver_psk[63] != '\0') {
			fprintf(stderr,
				"FAIL: P4C1 pinned driver PSK boundary\n");
			failures++;
		}
		p4c1[6 + 32 + 3] = '\0';
		wifi_input.pos = 0;
		if (entelea_wifi_read_p4c1(&wifi, read_input, &wifi_input) !=
			    -EINVAL ||
		    wifi.ssid_length != 0 || wifi.psk_length != 0) {
			fprintf(stderr,
				"FAIL: P4C1 embedded NUL must reject\n");
			failures++;
		}
	}
	{
		uint8_t p4c1[] = { 'P', '4', 'C', '1', 3,   8,	 'a', 'b', 'c',
				   '1', '2', '3', '4', '5', '6', '7', '8' };
		struct entelea_wifi_credentials wifi;
		struct input_stream wifi_input = { p4c1, sizeof(p4c1), 0 };

		memset(&wifi, 0xa5, sizeof(wifi));
		if (entelea_wifi_read_p4c1(&wifi, read_input, &wifi_input) !=
			    0 ||
		    wifi.ssid[3] != '\0' || wifi.psk[8] != '\0') {
			fprintf(stderr,
				"FAIL: short P4C1 fields must be terminated\n");
			failures++;
		}
		wifi_input.size = sizeof(p4c1) - 1;
		wifi_input.pos = 0;
		if (entelea_wifi_read_p4c1(&wifi, read_input, &wifi_input) !=
			    -ETIMEDOUT ||
		    wifi.ssid_length != 0 || wifi.psk_length != 0) {
			fprintf(stderr, "FAIL: P4C1 truncated credentials\n");
			failures++;
		}
	}
	if (failures != 0) {
		return 1;
	}
	puts("PASS: P4C1/P4C2 bounds, pinned driver strings and NUL rejection");
	return 0;
}
