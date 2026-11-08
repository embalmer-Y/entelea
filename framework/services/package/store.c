/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 包事实层（迁自旧 appmgr/pkg.c+slot.c+verify.c 的 R05 子集；合同见
 * framework/services/package/contract.md）。
 * 分步安装链（MA3.1）：stage_begin（擦）→ stage_chunk（顺序写+回读）→
 * verify（全量管线）→ activate（meta 记录 active）。
 * 验签：TSAP v2 头 + sha256(manifest‖wasm) 对拍 + COSE_Sign1 ed25519
 * （tweetnacl cr.yp.to 20140427 逐字节未改——crypto/PROVENANCE.md）+
 * 内嵌 payload 副本逐块比对（签 A 装 B 防御）。多根信任集（EC2）：
 * 工厂根（TEST=固定测试根 / 生产=prov pk0+pk1）∪ R03 信任表，任一过即过。
 * v1/未知 flags/域外 = fail-closed 拒收。active 事实经 meta 双副本。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/appstore.h>
#include <entelea/prov.h>
#include <entelea/storage.h>
#include <entelea/trust.h>
#include "../storage/internal.h"
#include "crypto/tweetnacl.h"

#define SLOT_SZ ENTELEA_APP_SLOT_SIZE

/* ENTELEA_PART_SLOT_A/B 在存储端口（part.c）——本层经公共端口访问。 */

entelea_result_t entelea_pkg_slot_read(uint8_t slot, uint32_t off, void *buf,
				       uint32_t len)
{
	if (slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	return entelea_storage_read((entelea_store_part_t)(
					    ENTELEA_PART_SLOT_A + slot),
				    off, buf, len);
}

entelea_result_t entelea_pkg_slot_write(uint8_t slot, uint32_t off,
					const void *buf, uint32_t len)
{
	if (slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	return entelea_storage_write((entelea_store_part_t)(
					     ENTELEA_PART_SLOT_A + slot),
				     off, buf, len);
}

entelea_result_t entelea_pkg_stage_begin(uint8_t slot)
{
	if (slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	return entelea_storage_erase((entelea_store_part_t)(
		ENTELEA_PART_SLOT_A + slot));
}

entelea_result_t entelea_pkg_stage_chunk(uint8_t slot, uint32_t off,
					 const void *buf, uint32_t len)
{
	if (buf == NULL || off > SLOT_SZ || len > SLOT_SZ - off) {
		return ENTELEA_E_RANGE;
	}
	if (entelea_pkg_slot_write(slot, off, buf, len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint8_t back[64];

	for (uint32_t done = 0; done < len; done += sizeof(back)) {
		uint32_t n = len - done;

		if (n > sizeof(back)) {
			n = sizeof(back);
		}
		if (entelea_pkg_slot_read(slot, off + done, back, n) != ENTELEA_OK ||
		    memcmp(back, (const uint8_t *)buf + done, n) != 0) {
			return ENTELEA_E_IO; /* 回读校验（RAM 后端不触发；flash 语义） */
		}
	}
	return ENTELEA_OK;
}

/* 固定测试根（旧 Q-29④ 公开材料；私钥在旧 Forge fixtures——测试信任
 * ≠生产信任）。 */
const uint8_t entelea_pkg_test_root_pub[32] = {
	0xf0, 0x84, 0xa4, 0xe5, 0x6f, 0x68, 0xb6, 0xab, 0x5c, 0xbe, 0xe8, 0x60,
	0x3c, 0x59, 0xa4, 0x2a, 0xc2, 0xef, 0x26, 0x51, 0xc4, 0x93, 0x94, 0xa7,
	0x03, 0x5c, 0xd5, 0xdb, 0x52, 0xdb, 0x47, 0x7a,
};

/* 有效信任集：工厂根（TEST=固定测试根/生产=prov 双钥）∪ 信任表。 */
static uint8_t trust_effective(const uint8_t *keys_out[ENTELEA_TRUST_SET_MAX])
{
	uint8_t n = 0;

#ifdef ENTELEA_TEST
	keys_out[n++] = entelea_pkg_test_root_pub;
#else
	keys_out[n++] = entelea_prov()->root_pubkeys[0];
	keys_out[n++] = entelea_prov()->root_pubkeys[1];
#endif
	const entelea_trust_table_t *t = entelea_trust();

	for (uint8_t i = 0; i < t->n_keys && n < ENTELEA_TRUST_SET_MAX; i++) {
		bool dup = false;

		for (uint8_t j = 0; j < n; j++) {
			if (memcmp(t->keys[i], keys_out[j], 32) == 0) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			keys_out[n++] = t->keys[i];
		}
	}
	return n;
}

/* definite 头元素（bstr/map）；短头/1B/2B/4B 扩展；不定长拒。 */
static entelea_result_t read_head(uint8_t slot, uint32_t off, uint8_t want_maj,
				  uint32_t *data_len, uint8_t *hdr_len)
{
	uint8_t b;

	if (entelea_pkg_slot_read(slot, off, &b, 1) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint8_t maj = b >> 5;
	uint8_t info = b & 0x1F;

	if (maj != want_maj || info == 31) {
		return ENTELEA_E_INVALID_SIG;
	}
	if (info < 24) {
		*data_len = info;
		*hdr_len = 1;
		return ENTELEA_OK;
	}
	uint8_t nb = (info == 24) ? 1 : (info == 25) ? 2 : (info == 26) ? 4 : 0;

	if (nb == 0) {
		return ENTELEA_E_INVALID_SIG; /* 8B 扩展超 u32 容器域 */
	}
	uint8_t lb[4] = {0, 0, 0, 0};

	if (entelea_pkg_slot_read(slot, off + 1, lb, nb) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint32_t v = 0;

	for (uint8_t i = 0; i < nb; i++) {
		v = (v << 8) | lb[i];
	}
	*data_len = v;
	*hdr_len = (uint8_t)(1 + nb);
	return ENTELEA_OK;
}

/* boot 快校验：头 v2 + 摘要重哈希（不含 ed25519——安装已全量验签，
 * boot 只防持久化位腐）。 */
static entelea_result_t check_digest(uint8_t slot, entelea_tsap_view_t *v)
{
	uint8_t hdr[ENTELEA_TSAP_HEADER_SIZE];

	if (entelea_pkg_slot_read(slot, 0, hdr, sizeof(hdr)) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (!entelea_tsap_parse_fields(hdr, v)) {
		return ENTELEA_E_INVALID_SIG;
	}
	if ((uint64_t)v->cose_off + 2 > (uint64_t)SLOT_SZ) {
		return ENTELEA_E_PARAM; /* 段布局越出 slot 物理面 */
	}
	uint8_t digest[ENTELEA_TSAP_DIGEST_SIZE];
	uint8_t buf[512];

	if (entelea_pkg_slot_read(slot, ENTELEA_TSAP_DIGEST_OFF, digest,
				  sizeof(digest)) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint32_t calc[8];
	entelea_sha256_stream_init(calc);
	uint32_t off = v->manifest_off;
	const uint32_t end = v->cose_off;

	while (off < end) {
		uint32_t n = end - off;

		if (n > sizeof(buf)) {
			n = sizeof(buf);
		}
		if (entelea_pkg_slot_read(slot, off, buf, n) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		entelea_sha256_stream_update(calc, buf, n);
		off += n;
	}
	uint8_t out[32];

	entelea_sha256_stream_final(calc, out);
	if (memcmp(out, digest, ENTELEA_TSAP_DIGEST_SIZE) != 0) {
		return ENTELEA_E_INVALID_SIG;
	}
	return ENTELEA_OK;
}

#define COSE_PHDR_LEN 3
#define SIG_STRUCTURE_PREFIX_MAX 22
#define VERIFY_SCRATCH (2 * (64 + SIG_STRUCTURE_PREFIX_MAX + 4096))

static size_t sig_structure_prefix(uint8_t *p, uint32_t payload_len)
{
	size_t pos = 0;

	p[pos++] = 0x84;
	p[pos++] = 0x6a;
	memcpy(p + pos, "Signature1", 10);
	pos += 10;
	p[pos++] = 0x43;
	p[pos++] = 0xa1;
	p[pos++] = 0x01;
	p[pos++] = 0x27;
	p[pos++] = 0x40;
	if (payload_len < 24) {
		p[pos++] = (uint8_t)(0x40 | payload_len);
	} else if (payload_len < 256) {
		p[pos++] = 0x58;
		p[pos++] = (uint8_t)payload_len;
	} else if (payload_len < 65536) {
		p[pos++] = 0x59;
		p[pos++] = (uint8_t)(payload_len >> 8);
		p[pos++] = (uint8_t)payload_len;
	} else {
		p[pos++] = 0x5A;
		p[pos++] = (uint8_t)(payload_len >> 24);
		p[pos++] = (uint8_t)(payload_len >> 16);
		p[pos++] = (uint8_t)(payload_len >> 8);
		p[pos++] = (uint8_t)payload_len;
	}
	return pos;
}

entelea_result_t entelea_pkg_verify(uint8_t slot, const uint8_t root_pubkey[32],
				    entelea_tsap_view_t *v, uint8_t digest_out[32])
{
	entelea_result_t r = check_digest(slot, v);

	if (r != ENTELEA_OK) {
		return r;
	}
	const uint8_t *set_buf[ENTELEA_TRUST_SET_MAX];
	uint8_t n_roots;

	if (root_pubkey == NULL) {
		n_roots = trust_effective(set_buf);
	} else {
		set_buf[0] = root_pubkey;
		n_roots = 1;
	}
	if (n_roots == 0) {
		return ENTELEA_E_INVALID_SIG;
	}
	if (digest_out != NULL &&
	    entelea_pkg_slot_read(slot, ENTELEA_TSAP_DIGEST_OFF, digest_out,
				  ENTELEA_TSAP_DIGEST_SIZE) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}

	/* COSE_Sign1 精确子集：tag18 + array4。 */
	uint8_t tag[2];

	if (entelea_pkg_slot_read(slot, v->cose_off, tag, 2) != ENTELEA_OK ||
	    tag[0] != 0xd2 || tag[1] != 0x84) {
		return ENTELEA_E_INVALID_SIG;
	}
	uint32_t pos = v->cose_off + 2;
	uint8_t phdr[COSE_PHDR_LEN];
	uint32_t el;
	uint8_t hl;

	r = read_head(slot, pos, 2, &el, &hl);
	if (r != ENTELEA_OK || el != COSE_PHDR_LEN) {
		return ENTELEA_E_INVALID_SIG;
	}
	if (entelea_pkg_slot_read(slot, pos + hl, phdr, el) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	static const uint8_t want_phdr[COSE_PHDR_LEN] = {0xa1, 0x01, 0x27};

	if (memcmp(phdr, want_phdr, COSE_PHDR_LEN) != 0) {
		return ENTELEA_E_INVALID_SIG;
	}
	pos += hl + el;

	r = read_head(slot, pos, 5, &el, &hl);
	if (r != ENTELEA_OK || el != 0 || hl != 1) {
		return ENTELEA_E_INVALID_SIG;
	}
	pos += hl;

	r = read_head(slot, pos, 2, &el, &hl);
	if (r != ENTELEA_OK || el != v->manifest_len + v->wasm_len) {
		return ENTELEA_E_INVALID_SIG;
	}
	const uint32_t payload_data = pos + hl;
	pos += hl + el;

	r = read_head(slot, pos, 2, &el, &hl);
	if (r != ENTELEA_OK || el != 64) {
		return ENTELEA_E_INVALID_SIG;
	}
	const uint32_t sig_off = pos + hl;

	const uint32_t payload_len = v->manifest_len + v->wasm_len;

	if (payload_len > 4096) {
		return ENTELEA_E_PARAM; /* 超本构建验签缓冲（档位随板调） */
	}
	static uint8_t scratch[VERIFY_SCRATCH];
	uint8_t *sm = scratch;
	uint8_t *mbuf = scratch + 64 + SIG_STRUCTURE_PREFIX_MAX + 4096;

	if (entelea_pkg_slot_read(slot, sig_off, sm, 64) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	size_t tail = 64 + sig_structure_prefix(sm + 64, payload_len);
	const size_t smlen = tail + payload_len;

	uint32_t coff = payload_data;
	uint32_t roff = v->manifest_off;
	uint32_t remain = payload_len;
	uint8_t raw[512];

	while (remain > 0) {
		uint32_t n = remain > sizeof(raw) ? (uint32_t)sizeof(raw) : remain;

		if (entelea_pkg_slot_read(slot, coff, raw, n) != ENTELEA_OK ||
		    entelea_pkg_slot_read(slot, roff, sm + tail, n) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		if (memcmp(raw, sm + tail, n) != 0) {
			return ENTELEA_E_INVALID_SIG; /* 内嵌≠裸段（签 A 装 B） */
		}
		coff += n;
		roff += n;
		tail += n;
		remain -= n;
	}

	unsigned long long mlen = 0;
	bool verified = false;

	for (uint8_t i = 0; i < n_roots; i++) {
		if (crypto_sign_open(mbuf, &mlen, sm, (unsigned long long)smlen,
				     set_buf[i]) == 0) {
			verified = true;
			break;
		}
	}
	if (!verified) {
		return ENTELEA_E_INVALID_SIG;
	}
	return ENTELEA_OK;
}

/* active 事实：meta 记录 "ACTV<slot>"（显式小态面；正式 meta 键空间
 * 归 R03 meta 消费约定——本层单键使用）。 */
entelea_result_t entelea_pkg_activate(uint8_t slot)
{
	if (slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	uint8_t rec[5] = {'A', 'C', 'T', 'V', (uint8_t)('0' + slot)};

	return entelea_storage_meta_write(rec, sizeof(rec));
}

entelea_result_t entelea_pkg_active(uint8_t *slot)
{
	if (slot == NULL) {
		return ENTELEA_E_PARAM;
	}
	uint8_t rec[ENTELEA_STORAGE_META_MAX];
	uint16_t len = 0;

	if (entelea_storage_meta_read(rec, &len) != ENTELEA_OK || len != 5 ||
	    memcmp(rec, "ACTV", 4) != 0 || rec[4] < '0' ||
	    rec[4] >= (uint8_t)('0' + ENTELEA_APP_SLOT_COUNT)) {
		return ENTELEA_E_UNINIT;
	}
	*slot = (uint8_t)(rec[4] - '0');
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
entelea_result_t entelea_pkg_test_reset(void)
{
	for (uint8_t s = 0; s < ENTELEA_APP_SLOT_COUNT; s++) {
		entelea_result_t r = entelea_storage_erase(
			(entelea_store_part_t)(ENTELEA_PART_SLOT_A + s));

		if (r != ENTELEA_OK) {
			return r;
		}
	}
	return ENTELEA_OK;
}
#endif
