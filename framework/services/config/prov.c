/* SPDX-License-Identifier: Apache-2.0 */
/*
 * provisioning 装载（迁自旧 store/prov.c，R03 合同见
 * framework/services/config/contract.md）。
 * PROV 分区布局（小端）：u32 cbor_len | u16 crc16(cbor) | cbor[]。
 * schema v1（9 对，定序）：v/node_id/cube_id/routers[tstr×≤4]/pk0/pk1/
 * cred/pwr_ma/estop；v2 = v1 前缀 + 尾部追加（11 对 deploy_mode+
 * estop_remote_clear；12 对 +lconf；15 对 +tls_ca/tls_cert/tls_key，
 * 三键同现同缺、len=0 键=结构损坏）。确定性子集解码：超集/不定长/
 * 域外值 = E_IO（fail-closed，不静默归并）。缺字段缺省 = orchestrated/
 * 远程清除开放/lconf 0/TLS off（存量 prov 零重刷兼容）。
 * 本文件零写调用（写通道 = 维护窗/maint 或烧录注入 prov_write_test，
 * 与 prov.c 分离——机械检查目标）。TLS 三元组 R03 起随装载态常驻
 * （旧 CONFIG_TS_NET_TLS 门控在 R06 消费方接入时复核足迹）。
 */
#include <string.h>
#include <entelea/prov.h>
#include <entelea/storage.h>
#include "internal.h"

#define PROV_CBOR_MAX 3800U /* 4KiB 分区内合理上限（结构性） */

/* 未装载/装载失败兜底 = v1 等义（estop_remote_clear 开放——测试构建常
 * 无 prov；装载失败生产板走 boot fail-safe，此值仅失败前观测安全侧）。 */
static entelea_prov_t prov = {
	.deploy_mode = ENTELEA_DEPLOY_ORCHESTRATED,
	.estop_remote_clear = 1,
};

static entelea_prov_tls_t prov_tls;

static entelea_result_t prov_walk(const uint8_t *cbor, uint32_t cbor_len,
				  entelea_prov_t *out, entelea_prov_tls_t *tls_out);

entelea_result_t entelea_prov_load(void)
{
	uint8_t hdr[6];

	if (entelea_storage_read(ENTELEA_PART_PROV, 0, hdr, sizeof(hdr)) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint32_t cbor_len = entelea_cfg_get_le32(hdr);
	uint16_t crc = entelea_cfg_get_le16(hdr + 4);

	if (cbor_len == 0 || cbor_len > PROV_CBOR_MAX) {
		return ENTELEA_E_IO;
	}
	static uint8_t cbor[PROV_CBOR_MAX];

	if (entelea_storage_read(ENTELEA_PART_PROV, sizeof(hdr), cbor,
				 cbor_len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_cfg_crc16(cbor, cbor_len) != crc) {
		return ENTELEA_E_IO; /* CRC 失败 → 调用方 fail-safe */
	}
	return prov_walk(cbor, cbor_len, &prov, &prov_tls);
}

static entelea_result_t prov_walk(const uint8_t *cbor, uint32_t cbor_len,
				  entelea_prov_t *out, entelea_prov_tls_t *tls_out)
{
	entelea_rd_t r = {.p = cbor, .rem = cbor_len};
	uint8_t ib;

	if (!entelea_rd_u8(&r, &ib) || (uint8_t)(ib & 0xE0) != 0xA0) {
		return ENTELEA_E_IO;
	}
	uint32_t pairs;

	/* v1=9 对；v2=11/12/15 对（键序=v1 前缀+尾部追加）。 */
	if (!entelea_rd_len(&r, ib & 0x1F, &pairs) ||
	    (pairs != 9 && pairs != 11 && pairs != 12 && pairs != 15)) {
		return ENTELEA_E_IO;
	}

	/* 缺字段缺省（先置再走查；out 可能为脏 scratch）。 */
	out->deploy_mode = ENTELEA_DEPLOY_ORCHESTRATED;
	out->estop_remote_clear = 1;
	out->local_confirm = 0;
	out->tls_ca_len = 0;
	out->tls_cert_len = 0;
	out->tls_key_len = 0;

	static const char *const keys[15] = {
		"v", "node_id", "cube_id", "routers", "pk0", "pk1", "cred",
		"pwr_ma", "estop", "deploy_mode", "estop_remote_clear", "lconf",
		"tls_ca", "tls_cert", "tls_key",
	};
	char key[20];
	uint32_t u;

	for (uint32_t k = 0; k < pairs; k++) {
		if (!entelea_rd_tstr(&r, key, sizeof(key)) ||
		    strcmp(key, keys[k]) != 0) {
			return ENTELEA_E_IO;
		}
		switch (k) {
		case 0: /* v：9 对 ⇔ v1，11/12/15 对 ⇔ v2（错配=结构损坏） */
			if (!entelea_rd_uint(&r, &u) ||
			    u != (uint32_t)(pairs == 9 ? 1 : 2)) {
				return ENTELEA_E_IO;
			}
			break;
		case 1:
			if (!entelea_rd_tstr(&r, out->node_id, sizeof(out->node_id))) {
				return ENTELEA_E_IO;
			}
			break;
		case 2:
			if (!entelea_rd_tstr(&r, out->cube_id, sizeof(out->cube_id))) {
				return ENTELEA_E_IO;
			}
			break;
		case 3: { /* routers: definite array of tstr ≤4 */
			uint8_t ab;

			if (!entelea_rd_u8(&r, &ab) || (uint8_t)(ab & 0xE0) != 0x80) {
				return ENTELEA_E_IO;
			}
			uint32_t n;

			if (!entelea_rd_len(&r, ab & 0x1F, &n) || n > 4) {
				return ENTELEA_E_IO;
			}
			for (uint32_t i = 0; i < n; i++) {
				if (!entelea_rd_tstr(&r, out->router_locators[i],
						     sizeof(out->router_locators[i]))) {
					return ENTELEA_E_IO;
				}
			}
			for (uint32_t i = n; i < 4; i++) {
				out->router_locators[i][0] = '\0';
			}
			break;
		}
		case 4:
			if (!entelea_rd_bstr(&r, out->root_pubkeys[0], 32)) {
				return ENTELEA_E_IO;
			}
			break;
		case 5:
			if (!entelea_rd_bstr(&r, out->root_pubkeys[1], 32)) {
				return ENTELEA_E_IO;
			}
			break;
		case 6:
			if (!entelea_rd_tstr(&r, out->zenoh_cred, sizeof(out->zenoh_cred))) {
				return ENTELEA_E_IO;
			}
			break;
		case 7:
			if (!entelea_rd_uint(&r, &out->power_budget_ma)) {
				return ENTELEA_E_IO;
			}
			break;
		case 8: /* estop 标志域 u8（防静默截断） */
			if (!entelea_rd_uint(&r, &u) || u > 0xFFU) {
				return ENTELEA_E_IO;
			}
			out->estop_trigger_flags = (uint8_t)u;
			break;
		case 9: /* deploy_mode：域外值 fail-closed */
			if (!entelea_rd_uint(&r, &u) ||
			    u > (uint32_t)ENTELEA_DEPLOY_STANDALONE) {
				return ENTELEA_E_IO;
			}
			out->deploy_mode = (uint8_t)u;
			break;
		case 10: /* estop_remote_clear：0|1 之外 fail-closed */
			if (!entelea_rd_uint(&r, &u) || u > 1U) {
				return ENTELEA_E_IO;
			}
			out->estop_remote_clear = (uint8_t)u;
			break;
		default:
			if (k == 11) { /* lconf：域外 fail-closed */
				if (!entelea_rd_uint(&r, &u) || u > 1U) {
					return ENTELEA_E_IO;
				}
				out->local_confirm = (uint8_t)u;
				break;
			}
			/* 12/13/14：TLS DER bstr；tls_out=NULL=validate 零拷贝。 */
			uint16_t blen = 0;
			uint8_t *dst = NULL;
			size_t cap = 0;
			uint16_t *dlen = NULL;

			switch (k) {
			case 12:
				dst = tls_out ? tls_out->ca : NULL;
				cap = ENTELEA_PROV_TLS_CA_MAX;
				dlen = &out->tls_ca_len;
				break;
			case 13:
				dst = tls_out ? tls_out->cert : NULL;
				cap = ENTELEA_PROV_TLS_CERT_MAX;
				dlen = &out->tls_cert_len;
				break;
			default:
				dst = tls_out ? tls_out->key : NULL;
				cap = ENTELEA_PROV_TLS_KEY_MAX;
				dlen = &out->tls_key_len;
				break;
			}
			if (!entelea_rd_bstr_len(&r, dst, cap, &blen) || blen == 0) {
				return ENTELEA_E_IO; /* 键在值空=结构损坏 */
			}
			*dlen = blen;
			if (tls_out != NULL) {
				/* 装载态自身长度镜像（消费者单一读面）。 */
				if (k == 12) {
					tls_out->ca_len = blen;
				} else if (k == 13) {
					tls_out->cert_len = blen;
				} else {
					tls_out->key_len = blen;
				}
			}
			break;
		}
	}
	if (r.rem != 0) {
		return ENTELEA_E_IO; /* 定长 schema：尾部余量=结构损坏 */
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_prov_validate(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len > PROV_CBOR_MAX) {
		return ENTELEA_E_PARAM;
	}
	entelea_prov_t scratch;

	/* 零拷贝走查（tls_out=NULL）：validate 不触碰装载活态。 */
	return prov_walk(cbor, len, &scratch, NULL);
}

const entelea_prov_t *entelea_prov(void)
{
	return &prov;
}

const entelea_prov_tls_t *entelea_prov_tls(void)
{
	return &prov_tls;
}
