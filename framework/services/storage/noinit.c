/* SPDX-License-Identifier: Apache-2.0 */
/*
 * noinit 留痕（迁自旧 store/noinit.c，R03 合同见本目录 contract.md）：
 * 布局（小端）u16 magic | u16 len | u16 crc16 | rec[]。
 * fresh 语义：有效记录 = 上次异常复位留痕存在（fresh=false）；空/损 =
 * fresh=true。one-shot：读后抹除（防陈旧留痕跨复位误报）。
 * R02 遗留接线落点：system_fail 复位原因经 safety 弱钩在此强实现落盘。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define NOINIT_MAGIC 0x5A5AU
#define NOINIT_HDR 6

/* init 消费结果（last_reset 查询面）。 */
static uint32_t last_reason;
static bool last_present;

void entelea_storage_last_reset(uint32_t *reason, bool *present)
{
	if (reason != NULL) {
		*reason = last_reason;
	}
	if (present != NULL) {
		*present = last_present;
	}
}

/* safety 弱钩强实现（force.c 弱桩）：system_fail 原因留痕。
 * 调用上下文 = system_fail（halt 后、停机前；RAM 后端写安全，
 * flash 后端接入时须复核该上下文可写 flash——板级批留痕）。 */
void entelea_safety_record_fail_reason(uint32_t reason)
{
	entelea_storage_noinit_put(&reason, sizeof(reason));
}

void entelea_storage_noinit_put(const void *rec, uint16_t len)
{
	if (rec == NULL || len == 0 || len > ENTELEA_STORAGE_NOINIT_MAX_REC) {
		return;
	}
	uint8_t buf[NOINIT_HDR + ENTELEA_STORAGE_NOINIT_MAX_REC];

	entelea_put_le16(buf, NOINIT_MAGIC);
	entelea_put_le16(buf + 2, len);
	entelea_put_le16(buf + 4, entelea_crc16(rec, len));
	memcpy(buf + NOINIT_HDR, rec, len);
	(void)entelea_store_backend.erase(ENTELEA_PART_NOINIT);
	(void)entelea_store_backend.write(ENTELEA_PART_NOINIT, 0, buf,
					  NOINIT_HDR + len);
}

entelea_result_t entelea_storage_noinit_get(void *rec, uint16_t *len, bool *fresh)
{
	if (rec == NULL || len == NULL || fresh == NULL) {
		return ENTELEA_E_PARAM;
	}
	uint8_t hdr[NOINIT_HDR];

	*fresh = true;
	if (entelea_store_backend.read(ENTELEA_PART_NOINIT, 0, hdr, sizeof(hdr)) !=
	    ENTELEA_OK) {
		return ENTELEA_OK; /* 读失败按无留痕（保守呈现，非伪造成功） */
	}
	if (entelea_get_le16(hdr) != NOINIT_MAGIC) {
		return ENTELEA_OK; /* 抹除态/无留痕 → fresh */
	}
	uint16_t rlen = entelea_get_le16(hdr + 2);

	if (rlen == 0 || rlen > ENTELEA_STORAGE_NOINIT_MAX_REC) {
		return ENTELEA_OK;
	}
	uint8_t data[ENTELEA_STORAGE_NOINIT_MAX_REC];

	if (entelea_store_backend.read(ENTELEA_PART_NOINIT, NOINIT_HDR, data,
				       rlen) != ENTELEA_OK) {
		return ENTELEA_OK;
	}
	if (entelea_crc16(data, rlen) != entelea_get_le16(hdr + 4)) {
		return ENTELEA_OK; /* 损坏视作无留痕 */
	}
	memcpy(rec, data, rlen);
	*len = rlen;
	*fresh = false;
	/* one-shot：读后抹除。 */
	(void)entelea_store_backend.erase(ENTELEA_PART_NOINIT);
	return ENTELEA_OK;
}

/* init 期消费（boot 步骤调用）：有留痕则记录 reason 供查询/上报。
 * 仅识别 u32 reason 形态（system_fail 编码）；其他长度照常消费不留。 */
entelea_result_t entelea_storage_init(void)
{
	uint8_t rec[ENTELEA_STORAGE_NOINIT_MAX_REC];
	uint16_t len = 0;
	bool fresh = true;

	last_present = false;
	last_reason = 0;
	if (entelea_storage_noinit_get(rec, &len, &fresh) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (!fresh && len == sizeof(uint32_t)) {
		last_reason = entelea_get_le32(rec);
		last_present = true;
	}
	/* 审计环游标恢复（分区缺失/扫描失败如实传递——观测面降级由
	 * 调用方处置；RAM 后端不触发）。 */
	return entelea_storage_audit_init();
}
