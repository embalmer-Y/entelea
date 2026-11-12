/* SPDX-License-Identifier: Apache-2.0 */
/* D12 日计数持久端口（Zephyr KV）。键与 PKG_ACTIVE_KV_TAG 同属 app_hash
 * 键空间（framework/services/package/store.c），标签为 ASCII "D12G"。 */
#include <entelea/d12.h>
#include <entelea/kv.h>
#include "d12_ports.h"

#define D12_KV_TAG 0x44313247U

static entelea_result_t d12_store_load(void *user,
				       uint8_t rec[ENTELEA_D12_REC_LEN])
{
	int32_t n;

	(void)user;
	n = entelea_kv_load(D12_KV_TAG, rec, ENTELEA_D12_REC_LEN);
	if (n < 0) {
		return (entelea_result_t)n;
	}
	return n == (int32_t)ENTELEA_D12_REC_LEN ? ENTELEA_OK : ENTELEA_E_STATE;
}

static entelea_result_t d12_store_save(void *user,
				       const uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void)user;
	return entelea_kv_save(D12_KV_TAG, rec, ENTELEA_D12_REC_LEN);
}

entelea_result_t entelea_platform_d12_store_init(entelea_d12_store_t *store)
{
	entelea_result_t r;

	if (store == NULL) {
		return ENTELEA_E_PARAM;
	}
	r = entelea_kv_init();
	if (r != ENTELEA_OK) {
		return r;
	}
	store->load = d12_store_load;
	store->save = d12_store_save;
	store->user = NULL;
	return ENTELEA_OK;
}
