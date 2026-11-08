/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 掉电安全 meta（迁自旧 store/meta.c，R03 合同见本目录 contract.md）：
 * 双副本 + 单调序号 + CRC16。撕裂恢复：活动 = 序号较大且 CRC 通过者；
 * 双副本皆损 → ENTELEA_E_IO（不伪造缺省成功）。写序：范围抹除非活动
 * 副本 → 写 → 回读校验 → 完成即成为新活动（撕裂时旧副本仍完整可读）。
 * 沿用旧 MD1.1b 修复：读取两副本体用独立缓冲（共用缓冲曾使 copy0 较新
 * 时读回 copy1 旧内容——第 3/5/… 次写后启动 100% 复现）。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define REC_HDR 8 /* seq u32 | len u16 | crc16 u16（小端） */

static uint32_t meta_stride(void)
{
	return entelea_store_backend.size(ENTELEA_PART_META) / 2;
}

static entelea_result_t read_rec(uint8_t copy, uint32_t *seq, uint16_t *len,
				 uint8_t *data)
{
	uint8_t hdr[REC_HDR];
	uint32_t stride = meta_stride();

	if (stride < REC_HDR + ENTELEA_STORAGE_META_MAX ||
	    entelea_store_backend.read(ENTELEA_PART_META, copy * stride, hdr,
				       REC_HDR) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	*seq = entelea_get_le32(hdr);
	*len = entelea_get_le16(hdr + 4);
	uint16_t crc = entelea_get_le16(hdr + 6);

	if (*len == 0 || *len > ENTELEA_STORAGE_META_MAX) {
		return ENTELEA_E_IO;
	}
	if (entelea_store_backend.read(ENTELEA_PART_META, copy * stride + REC_HDR,
				       data, *len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_crc16(data, *len) != crc) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

static entelea_result_t write_rec(uint8_t copy, uint32_t seq, const void *buf,
				  uint16_t len)
{
	uint8_t rec[REC_HDR + ENTELEA_STORAGE_META_MAX];
	uint32_t stride = meta_stride();

	if (stride < REC_HDR + len) {
		return ENTELEA_E_RANGE;
	}
	entelea_put_le32(rec, seq);
	entelea_put_le16(rec + 4, len);
	entelea_put_le16(rec + 6, entelea_crc16(buf, len));
	memcpy(rec + REC_HDR, buf, len);
	uint32_t total = REC_HDR + len;

	/* 擦-写之间撕裂 → 该副本损，另一副本仍完整（安全性保持）；
	 * RAM 后端同序无害。 */
	if (entelea_store_backend.erase_off(ENTELEA_PART_META, copy * stride,
					    stride) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_store_backend.write(ENTELEA_PART_META, copy * stride, rec,
					total) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	/* 校验回读 */
	uint8_t back[REC_HDR + ENTELEA_STORAGE_META_MAX];

	if (entelea_store_backend.read(ENTELEA_PART_META, copy * stride, back,
				       total) != ENTELEA_OK ||
	    memcmp(rec, back, total) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_storage_meta_write(const void *buf, uint16_t len)
{
	if (buf == NULL || len == 0 || len > ENTELEA_STORAGE_META_MAX) {
		return ENTELEA_E_PARAM;
	}
	uint8_t data[ENTELEA_STORAGE_META_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	bool ok0 = read_rec(0, &s0, &l0, data) == ENTELEA_OK;
	bool ok1 = read_rec(1, &s1, &l1, data) == ENTELEA_OK;

	uint32_t cur_seq = 0;
	uint8_t inactive = 0;

	if (ok0 && ok1) {
		cur_seq = (s0 >= s1) ? s0 : s1;
		inactive = (s0 >= s1) ? 1 : 0;
	} else if (ok0) {
		cur_seq = s0;
		inactive = 1;
	} else if (ok1) {
		cur_seq = s1;
		inactive = 0;
	}
	return write_rec(inactive, cur_seq + 1, buf, len);
}

entelea_result_t entelea_storage_meta_read(void *buf, uint16_t *len)
{
	if (buf == NULL || len == NULL) {
		return ENTELEA_E_PARAM;
	}
	/* 双副本独立缓冲（MD1.1b：共用缓冲在读 copy0 较新时返回 copy1 旧值）。 */
	uint8_t data0[ENTELEA_STORAGE_META_MAX];
	uint8_t data1[ENTELEA_STORAGE_META_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	bool ok0 = read_rec(0, &s0, &l0, data0) == ENTELEA_OK;
	bool ok1 = read_rec(1, &s1, &l1, data1) == ENTELEA_OK;

	if (!ok0 && !ok1) {
		return ENTELEA_E_IO; /* 双副本皆损：缺省安全态/fail-safe（不伪造） */
	}
	if (ok0 && (!ok1 || s0 >= s1)) {
		memcpy(buf, data0, l0);
		*len = l0;
	} else {
		memcpy(buf, data1, l1);
		*len = l1;
	}
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
entelea_result_t entelea_storage_meta_corrupt_test(uint8_t copy_idx)
{
	if (copy_idx > 1) {
		return ENTELEA_E_PARAM;
	}
	uint32_t stride = meta_stride();

	if (entelea_store_backend.erase_off(ENTELEA_PART_META, copy_idx * stride,
					    stride) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint8_t garbage[REC_HDR] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};

	return entelea_store_backend.write(ENTELEA_PART_META, copy_idx * stride,
					   garbage, REC_HDR);
}
#endif
