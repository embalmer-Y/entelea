/* SPDX-License-Identifier: Apache-2.0 */
/*
 * prov 维护写通道（V0.4.0 M10，Q-34/DEC-53）：zenoh sys/prov-write 消费。
 * prov.c 本体零写不变（L5 检查目标）——本文件与 prov_test.c 同型，为
 * 「烧录期语义」的远程形态：写入前全量 schema 校验（fail-closed——校验
 * 失败零写入）+ **per-boot 一次性窗**（RAM 旗：本 boot 已写过 = 拒绝，
 * 再写须经一次复位循环——远程反复换身份被结构性限速且每次可观测）。
 * 记录格式与 prov_test.c 一致（头 len|crc + CBOR 一体单次连续写）。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <ts/store.h>
#include "internal.h"

static bool maint_written_this_boot;

ts_res_t ts_store_prov_write_maint(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len + 6 > 4096) {
		return TS_E_PARAM;
	}
	if (maint_written_this_boot) {
		return TS_E_STATE; /* 一次性窗：再写须复位（Q-34） */
	}
	/* 先校验后写（schema 走查与 load 同路径——单一事实源） */
	ts_res_t v = ts_store_prov_validate(cbor, len);

	if (v != TS_OK) {
		return v; /* 非法 prov = 零写入（fail-closed） */
	}
	static uint8_t rec[4096];

	ts_put_le32(rec, len);
	ts_put_le16(rec + 4, ts_crc16(cbor, len));
	memcpy(rec + 6, cbor, len);
	if (ts_store_backend.erase(TS_PART_PROV) != TS_OK) {
		return TS_E_IO;
	}
	if (ts_store_backend.write(TS_PART_PROV, 0, rec, 6 + len) != TS_OK) {
		return TS_E_IO;
	}
	maint_written_this_boot = true;
	return TS_OK;
}

#ifdef CONFIG_TS_TEST
void ts_store_prov_maint_test_reset(void)
{
	maint_written_this_boot = false;
}
#endif
