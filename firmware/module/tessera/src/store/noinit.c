/* SPDX-License-Identifier: Apache-2.0 */
/*
 * noinit 留痕（LLD-ts-store §5）：复位原因 / WDT 报告（消费者 ts-core，DR-17）。
 * 布局（小端）：u16 magic=0x5A5A | u16 len | u16 crc16 | rec[]
 * fresh 语义：读到有效记录 = 上次异常复位留痕存在（fresh=false）；空/损 = fresh=true。
 */
#include <string.h>
#include <ts/store.h>
#include "internal.h"

#define NOINIT_MAGIC   0x5A5AU
#define NOINIT_HDR     6
#define NOINIT_MAX_REC 250 /* 分区 256B 内（结构性） */

/* DEC-48 L4（Q-28④）：system_fail 复位原因留痕——强实现覆盖 force.c 弱桩
 * （TS_STORE=n 构建回落弱桩）。reason = TS_FAIL_* 编码（safety.h）。 */
void ts_store_noinit_record(uint32_t reason)
{
	ts_store_noinit_put(&reason, sizeof(reason));
}

void ts_store_noinit_put(const void *rec, uint16_t len)
{
	if (rec == NULL || len == 0 || len > NOINIT_MAX_REC) {
		return;
	}
	uint8_t buf[NOINIT_HDR + NOINIT_MAX_REC];

	ts_put_le16(buf, NOINIT_MAGIC);
	ts_put_le16(buf + 2, len);
	ts_put_le16(buf + 4, ts_crc16(rec, len));
	memcpy(buf + NOINIT_HDR, rec, len);
	(void)ts_store_backend.erase(TS_PART_NOINIT);
	(void)ts_store_backend.write(TS_PART_NOINIT, 0, buf, NOINIT_HDR + len);
}

ts_res_t ts_store_noinit_get(void *rec, uint16_t *len, bool *fresh)
{
	if (rec == NULL || len == NULL || fresh == NULL) {
		return TS_E_PARAM;
	}
	uint8_t hdr[NOINIT_HDR];

	*fresh = true;
	if (ts_store_backend.read(TS_PART_NOINIT, 0, hdr, sizeof(hdr)) != TS_OK) {
		return TS_OK; /* 读失败按无留痕（保守） */
	}
	if (ts_get_le16(hdr) != NOINIT_MAGIC) {
		return TS_OK; /* 抹除态/无留痕 → fresh */
	}
	uint16_t rlen = ts_get_le16(hdr + 2);

	if (rlen == 0 || rlen > NOINIT_MAX_REC) {
		return TS_OK;
	}
	uint8_t data[NOINIT_MAX_REC];

	if (ts_store_backend.read(TS_PART_NOINIT, NOINIT_HDR, data, rlen) != TS_OK) {
		return TS_OK;
	}
	if (ts_crc16(data, rlen) != ts_get_le16(hdr + 4)) {
		return TS_OK; /* 损坏视作无留痕 */
	}
	memcpy(rec, data, rlen);
	*len = rlen;
	*fresh = false; /* 上次复位前留痕存在（异常复位可观测） */
	/* one-shot：读后抹除。flash 持久介质上防陈旧留痕跨多次复位误报
	 *（RAM 后端同语义统一——此前 RAM 掩盖了该缺口，板级五 flash 后端
	 * 暴露后收敛；LLD-ts-store §5 语义细化）。 */
	(void)ts_store_backend.erase(TS_PART_NOINIT);
	return TS_OK;
}
