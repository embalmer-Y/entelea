/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_ZEPHYR_WIFI_CREDENTIALS_H
#define ENTELEA_ZEPHYR_WIFI_CREDENTIALS_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ENTELEA_WIFI_SSID_MAX 32U
#define ENTELEA_WIFI_PSK_MAX 63U

struct entelea_wifi_credentials {
	uint8_t ssid[ENTELEA_WIFI_SSID_MAX + 1U];
	uint8_t psk[ENTELEA_WIFI_PSK_MAX + 1U];
	uint8_t ssid_length;
	uint8_t psk_length;
};

typedef int (*entelea_credential_read_fn)(void *user, uint8_t *dst,
					  size_t size);

static inline int
entelea_credential_read_text(entelea_credential_read_fn read, void *user,
			     uint8_t *dst, size_t capacity, uint8_t len)
{
	int ret;

	if (len >= capacity) {
		return -EINVAL;
	}
	ret = read(user, dst, len);
	if (ret != 0) {
		return ret;
	}
	if (memchr(dst, '\0', len) != NULL) {
		return -EINVAL;
	}
	dst[len] = '\0';
	return 0;
}

static inline int
entelea_wifi_read_fields(struct entelea_wifi_credentials *c, uint8_t ssid_len,
			 uint8_t psk_len, entelea_credential_read_fn read,
			 void *user)
{
	int ret;

	if (c == NULL || read == NULL || ssid_len == 0 ||
	    ssid_len > ENTELEA_WIFI_SSID_MAX || psk_len < 8 ||
	    psk_len > ENTELEA_WIFI_PSK_MAX) {
		return -EINVAL;
	}
	c->ssid_length = 0;
	c->psk_length = 0;
	ret = entelea_credential_read_text(read, user, c->ssid, sizeof(c->ssid),
					   ssid_len);
	if (ret != 0) {
		return ret;
	}
	ret = entelea_credential_read_text(read, user, c->psk, sizeof(c->psk),
					   psk_len);
	if (ret != 0) {
		return ret;
	}
	c->ssid_length = ssid_len;
	c->psk_length = psk_len;
	return 0;
}

static inline int
entelea_wifi_read_p4c1(struct entelea_wifi_credentials *c,
		       entelea_credential_read_fn read, void *user)
{
	uint8_t header[6];
	int ret;

	if (c == NULL || read == NULL) {
		return -EINVAL;
	}
	ret = read(user, header, sizeof(header));
	if (ret != 0) {
		return ret;
	}
	if (memcmp(header, "P4C1", 4) != 0) {
		return -EINVAL;
	}
	return entelea_wifi_read_fields(c, header[4], header[5], read, user);
}

#endif /* ENTELEA_ZEPHYR_WIFI_CREDENTIALS_H */
