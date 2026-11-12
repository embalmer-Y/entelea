/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HW_CLOUD_P4C2_H
#define ENTELEA_HW_CLOUD_P4C2_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../wifi_credentials.h"

struct gate_credentials {
	struct entelea_wifi_credentials wifi;
	char host[64];
	char api_key[128];
	char model[48];
};

static inline int entelea_p4c2_read(struct gate_credentials *c,
				    entelea_credential_read_fn read, void *user)
{
	uint8_t header[9];
	int ret;

	if (c == NULL || read == NULL) {
		return -EINVAL;
	}
	ret = read(user, header, sizeof(header));
	if (ret != 0) {
		return ret;
	}
	if (memcmp(header, "P4C2", 4) || !header[4] ||
	    header[4] > ENTELEA_WIFI_SSID_MAX || header[5] < 8 ||
	    header[5] > ENTELEA_WIFI_PSK_MAX || !header[6] || header[6] > 63 ||
	    header[7] < 20 || header[7] > 127 || !header[8] || header[8] > 47) {
		return -EINVAL;
	}
	ret = entelea_wifi_read_fields(&c->wifi, header[4], header[5], read, user);
	if (ret != 0) {
		return ret;
	}
	ret = entelea_credential_read_text(read, user, (uint8_t *)c->host,
					   sizeof(c->host), header[6]);
	if (ret != 0) {
		return ret;
	}
	ret = entelea_credential_read_text(read, user, (uint8_t *)c->api_key,
					   sizeof(c->api_key), header[7]);
	if (ret != 0) {
		return ret;
	}
	return entelea_credential_read_text(read, user, (uint8_t *)c->model,
					    sizeof(c->model), header[8]);
}

#endif /* ENTELEA_HW_CLOUD_P4C2_H */
