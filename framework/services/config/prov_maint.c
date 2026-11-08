/* SPDX-License-Identifier: Apache-2.0 */
/*
 * prov 维护写通道 + 烧录测试注入（迁自旧 store/prov_maint.c 与
 * prov_test.c，R03 合同见 framework/services/config/contract.md）。
 * prov.c 本体零写不变（机械检查目标）。维护窗：写入前全量 schema 校验
 * （fail-closed——失败零写入）+ per-boot 一次性窗（RAM 旗：再写须经
 * 复位循环——远程换身份结构性限速且可观测）。记录格式与烧录注入一致
 * （头 len|crc + CBOR 一体单次连续写——flash 程序一次纪律）。
 */
#include <string.h>
#include <entelea/prov.h>
#include <entelea/storage.h>
#include "internal.h"

static bool maint_written_this_boot;

entelea_result_t entelea_prov_write_maint(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len + 6 > 4096) {
		return ENTELEA_E_PARAM;
	}
	if (maint_written_this_boot) {
		return ENTELEA_E_STATE; /* 一次性窗：再写须复位 */
	}
	entelea_result_t v = entelea_prov_validate(cbor, len);

	if (v != ENTELEA_OK) {
		return v; /* 非法 prov = 零写入（fail-closed） */
	}
	static uint8_t rec[4096];

	entelea_cfg_put_le32(rec, len);
	entelea_cfg_put_le16(rec + 4, entelea_cfg_crc16(cbor, len));
	memcpy(rec + 6, cbor, len);
	if (entelea_storage_erase(ENTELEA_PART_PROV) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_storage_write(ENTELEA_PART_PROV, 0, rec, 6 + len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	maint_written_this_boot = true;
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_prov_maint_test_reset(void)
{
	maint_written_this_boot = false;
}

entelea_result_t entelea_prov_write_test(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len + 6 > 4096) {
		return ENTELEA_E_PARAM;
	}
	static uint8_t rec[4096];

	entelea_cfg_put_le32(rec, len);
	entelea_cfg_put_le16(rec + 4, entelea_cfg_crc16(cbor, len));
	memcpy(rec + 6, cbor, len);
	if (entelea_storage_erase(ENTELEA_PART_PROV) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_storage_write(ENTELEA_PART_PROV, 0, rec, 6 + len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}
#endif
