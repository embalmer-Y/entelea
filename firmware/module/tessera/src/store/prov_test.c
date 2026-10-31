/* SPDX-License-Identifier: Apache-2.0 */
/*
 * prov 烧录通道测试注入（仅 CONFIG_TS_TEST 编译）——prov.c 本体保持零写调用
 * （L5 机械检查目标，LLD-ts-store §4）。生产烧录 = 外部工具 / Agent
 * deploy_push_prov（MA3）。
 */
#include <string.h>
#include <ts/store.h>
#include "internal.h"

#ifdef CONFIG_TS_TEST
ts_res_t ts_store_prov_write_test(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len + 6 > 4096) {
		return TS_E_PARAM;
	}
	/* 头（len u32 | crc16 u16，小端）+ CBOR 一体单次连续写：flash 后端
	 * 程序一次纪律——分区 erase 后每字至多一次 program（跨写边界字重编程
	 * 在 sim-flash EXPLICIT_ERASE 语义下被拒，真机亦损寿命；prov.c 本体
	 * 仍零写调用，L5 目标不变）。 */
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
	return TS_OK;
}
#endif
