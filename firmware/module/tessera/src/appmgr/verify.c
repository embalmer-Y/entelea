/* SPDX-License-Identifier: Apache-2.0 */
/* TSAP v2 全量验签管线（DEC-49①②④；IR2-02 根治）。
 * 管线：头 v2（flags=1）→ sha256(manifest‖wasm) == 头摘要 → COSE_Sign1
 * ed25519 验签（tweetnacl；Sig_structure = ["Signature1", phdr, bstr"",
 * payload]——RFC 9052 §4.2，与 Agent 侧 cose.py DIY 实现同构，字节级对拍）。
 * 签名覆盖 COSE 内嵌 payload 副本——本管线同时逐块比对内嵌副本与裸段
 * （manifest‖wasm）一致，防"签 A 装 B"。
 * TEST 语义（Q-29④）：CONFIG_TS_TEST 构建验签对象 = 固定测试根（本文件
 * const；密钥对 = agent/tests/fixtures/ts-test-root——测试信任 ≠ 生产信任，
 * 公开材料）；生产构建 = 调用方传入根公钥（prov pk0）。
 * boot 快校验（ts_appmgr_check_digest）：头 + 摘要重哈希（不含 ed25519——
 * 安装/激活已全量验签，boot 只需防持久化位腐；毫秒级）。
 * 失败路径 printk 行号留痕（低频——仅失败安装；可观测性）。 */
#include <string.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/store.h>
#include <ts/tsap.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#if defined(CONFIG_TS_APP_PSRAM_VERIFY_BUF) && defined(CONFIG_SHARED_MULTI_HEAP)
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif
#include "crypto/tweetnacl.h"
#include "internal.h"
/* sha256 复用 ts-store 实现（src/store/sha256.c 同 API；DEC-49②） */
#include "../store/internal.h"

#define VS_REJECT(code) \
	do { printk("[vrf] reject L%d\n", __LINE__); return (code); } while (0)

/* 固定测试根公钥（Q-29④；生成：~/project/logs/gen_test_root.py——
 * 密钥对仓内 agent/tests/fixtures/ts-test-root.{key,pub}，测试专用公开材料） */
static const uint8_t test_root_pub[32] = {
	0xf0, 0x84, 0xa4, 0xe5, 0x6f, 0x68, 0xb6, 0xab, 0x5c, 0xbe, 0xe8, 0x60,
	0x3c, 0x59, 0xa4, 0x2a, 0xc2, 0xef, 0x26, 0x51, 0xc4, 0x93, 0x94, 0xa7,
	0x03, 0x5c, 0xd5, 0xdb, 0x52, 0xdb, 0x47, 0x7a,
};

const uint8_t *ts_appmgr_root_key(const uint8_t root_pubkey[32])
{
	if (IS_ENABLED(CONFIG_TS_TEST)) {
		return test_root_pub; /* TEST 语义：固定测试根（防未签名+自声明） */
	}
	return root_pubkey; /* 生产：调用方传 prov pk0（NULL = 拒绝） */
}

/* ---- 有效信任集（EC2 · Q-43②/DEC-60②）------------------------------------
 * 有效集 = 工厂根 ∪ 信任表 keys。工厂根：TEST = 固定测试根（Q-29④）/
 * 生产 = prov pk0+pk1（pk1 自 EC2 转正消费——schema v1 既存双钥预留）。
 * 工厂根不受信任表管辖（吊销 = prov 维护通道职权）；有效集恒非空。 */

uint8_t ts_appmgr_trust_effective(const uint8_t *keys_out[TS_APPMGR_TRUST_SET_MAX])
{
	uint8_t n = 0;

	if (IS_ENABLED(CONFIG_TS_TEST)) {
		keys_out[n++] = test_root_pub;
	} else {
		const ts_prov_t *prov = ts_store_prov();

		keys_out[n++] = prov->root_pubkeys[0];
		keys_out[n++] = prov->root_pubkeys[1];
	}
#ifdef CONFIG_TS_STORE_TRUST
	const ts_trust_table_t *t = ts_store_trust();

	for (uint8_t i = 0; i < t->n_keys && n < TS_APPMGR_TRUST_SET_MAX; i++) {
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
#endif
	return n;
}

static ts_res_t read_all(uint8_t slot, uint32_t off, uint8_t *buf, uint32_t len)
{
	return ts_store_slot_read(slot, off, buf, len);
}

/* 读一个 definite 头元素（bstr/map）；返回数据长度与头占用字节数。
 * fail-closed 子集：短头/1B/2B/4B 扩展（K5：large 档 COSE payload bstr
 * 达 200KB+——DEC-49 期「≤64KB 容器域」约束随大档解除；8B 扩展超 u32
 * 域仍拒），不定长 = 拒绝。 */
static ts_res_t read_head(uint8_t slot, uint32_t off, uint8_t want_maj,
			  uint32_t *data_len, uint8_t *hdr_len)
{
	uint8_t b;

	if (read_all(slot, off, &b, 1) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}
	uint8_t maj = b >> 5;
	uint8_t info = b & 0x1F;

	if (maj != want_maj || info == 31) {
		VS_REJECT(TS_E_INVALID_SIG); /* 非 目标类型 / 不定长 = 拒绝 */
	}
	if (info < 24) {
		*data_len = info;
		*hdr_len = 1;
		return TS_OK;
	}
	uint8_t nb = (info == 24) ? 1 : (info == 25) ? 2 : (info == 26) ? 4 : 0;

	if (nb == 0) {
		VS_REJECT(TS_E_INVALID_SIG); /* 8B 扩展超 u32 容器域 */
	}
	uint8_t lb[4] = {0, 0, 0, 0};

	if (read_all(slot, off + 1, lb, nb) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}
	uint32_t v = 0;

	for (uint8_t i = 0; i < nb; i++) {
		v = (v << 8) | lb[i];
	}
	*data_len = v;
	*hdr_len = (uint8_t)(1 + nb);
	return TS_OK;
}

/* ---- 摘要快校验（boot 路径；view 填充含 v2 布局）-------------------------- */

ts_res_t ts_appmgr_check_digest(uint8_t slot, tsap_view_t *v)
{
	uint8_t hdr[TSAP_HEADER_SIZE];

	if (read_all(slot, 0, hdr, sizeof(hdr)) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}
	if (!tsap_header_parse_fields(hdr, v)) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	if ((uint64_t)v->cose_off + 2 > (uint64_t)CONFIG_TS_STORE_SLOT_SIZE) {
		VS_REJECT(TS_E_PARAM); /* 段布局越出 slot 物理面 */
	}
	uint8_t digest[TSAP_DIGEST_SIZE];
	uint8_t buf[512];
	ts_sha256_ctx_t sc;

	if (read_all(slot, TSAP_DIGEST_OFF, digest, sizeof(digest)) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}
	ts_sha256_init(&sc);
	uint32_t off = v->manifest_off;
	const uint32_t end = v->cose_off;

	while (off < end) {
		uint32_t n = end - off;

		if (n > sizeof(buf)) {
			n = sizeof(buf);
		}
		if (read_all(slot, off, buf, n) != TS_OK) {
			VS_REJECT(TS_E_IO);
		}
		ts_sha256_update(&sc, buf, n);
		off += n;
	}
	uint8_t calc[TSAP_DIGEST_SIZE];

	ts_sha256_final(&sc, calc);
	if (memcmp(calc, digest, TSAP_DIGEST_SIZE) != 0) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	return TS_OK;
}

/* ---- 全量验签（安装/激活/回滚路径）---------------------------------------- */

/* COSE_Sign1 protected 精确子集：bstr(map{1:-8}) = {0x43, 0xa1, 0x01, 0x27}
 * （DR-21 单键纪律——Agent 侧同源约束） */
#define COSE_PHDR_LEN 3

/* sm 布局：sig(64) ‖ Sig_structure 定前缀（canonical：array4 + tstr(10)
 * "Signature1" + bstr(3) phdr + bstr"" + bstr(payload) 按长选头）；
 * payload 体随后逐块追加。前缀上界 = 1+11+4+1+5 = 22（K5：≥64KB 取
 * 0x5A 4B 头——与 Agent 侧 canonical 编码一致；此前 2B 截断形两边
 * Sig_structure 不一致 = 大包验签必败，真机 L359 定位）。 */
#define SIG_STRUCTURE_PREFIX_MAX 22

static size_t sig_structure_prefix(uint8_t *p, uint32_t payload_len)
{
	size_t pos = 0;

	p[pos++] = 0x84; /* array(4) */
	p[pos++] = 0x6a; /* tstr(10) */
	memcpy(p + pos, "Signature1", 10);
	pos += 10;
	p[pos++] = 0x43; /* bstr(3) = phdr {1:-8} */
	p[pos++] = 0xa1;
	p[pos++] = 0x01;
	p[pos++] = 0x27;
	p[pos++] = 0x40; /* bstr("") = external aad */
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

ts_res_t ts_appmgr_verify_slot(uint8_t slot, const uint8_t root_pubkey[32],
			       tsap_view_t *v, uint8_t digest_out[32])
{
	ts_res_t r = ts_appmgr_check_digest(slot, v);

	if (r != TS_OK) {
		return r; /* 头 v2/摘要/内容完整性（含 view 填充）——上方已留痕 */
	}
	/* EC2（Q-43⑤）：NULL = 迭代有效信任集（工厂根 ∪ 信任表——任一键过
	 * 即过；rollback/boot 装载链走此路）；显式键 = 单键验签（TEST 构建
	 * 维持固定测试根覆盖——Q-29④ 现行为）。 */
	const uint8_t *set_buf[TS_APPMGR_TRUST_SET_MAX];
	uint8_t n_roots;

	if (root_pubkey == NULL) {
		n_roots = ts_appmgr_trust_effective(set_buf);
	} else {
		set_buf[0] = ts_appmgr_root_key(root_pubkey);
		n_roots = 1;
	}
	if (n_roots == 0) {
		VS_REJECT(TS_E_INVALID_SIG); /* 防御：有效集空（结构不可达） */
	}
	if (digest_out != NULL &&
	    read_all(slot, TSAP_DIGEST_OFF, digest_out, TSAP_DIGEST_SIZE) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}

	/* COSE_Sign1 逐元素解析（tag18 + array4 精确子集） */
	uint8_t tag[2];

	if (read_all(slot, v->cose_off, tag, 2) != TS_OK ||
	    tag[0] != 0xd2 || tag[1] != 0x84) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	uint32_t pos = v->cose_off + 2;
	uint8_t phdr[COSE_PHDR_LEN];
	uint32_t el;
	uint8_t hl;

	r = read_head(slot, pos, 2, &el, &hl); /* ① protected bstr */
	if (r != TS_OK || el != COSE_PHDR_LEN) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	if (read_all(slot, pos + hl, phdr, el) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}
	static const uint8_t want_phdr[COSE_PHDR_LEN] = {0xa1, 0x01, 0x27};

	if (memcmp(phdr, want_phdr, COSE_PHDR_LEN) != 0) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	pos += hl + el;

	r = read_head(slot, pos, 5, &el, &hl); /* ② unprotected map(0) */
	if (r != TS_OK || el != 0 || hl != 1) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	pos += hl;

	r = read_head(slot, pos, 2, &el, &hl); /* ③ payload bstr */
	if (r != TS_OK || el != v->manifest_len + v->wasm_len) {
		VS_REJECT(TS_E_INVALID_SIG); /* 签名载荷必须覆盖全 manifest‖wasm */
	}
	const uint32_t payload_data = pos + hl;
	pos += hl + el;

	r = read_head(slot, pos, 2, &el, &hl); /* ④ signature bstr */
	if (r != TS_OK || el != 64) {
		VS_REJECT(TS_E_INVALID_SIG);
	}
	const uint32_t sig_off = pos + hl;

	/* sm = sig(64) ‖ Sig_structure；payload 自 COSE 内嵌副本逐块读入，
	 * 同时与裸段逐块比对（签名面 == 存储面）。 */
	/* 单 scratch 双区：sm 区（sig‖Sig_structure）+ m 区（tweetnacl 出参，
	 * 独立不别名——见下方调用点注释）。
	 * K5：large 档板 TS_VERIFY_SCRATCH（≥2×容器）DRAM 静态不可行——
	 * TS_APP_PSRAM_VERIFY_BUF = SMH 外部 RAM 懒分配同型（TS_APP_PSRAM_LOAD_BUF）。
	 * 调用上下文 = 部署会话（gated 命令面，net 读任务线程）——单线程串行。 */
#if defined(CONFIG_TS_APP_PSRAM_VERIFY_BUF) && defined(CONFIG_SHARED_MULTI_HEAP)
	static uint8_t *scratch;

	if (scratch == NULL) {
		scratch = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL,
						 CONFIG_TS_VERIFY_SCRATCH);
		if (scratch == NULL) {
			VS_REJECT(TS_E_NOMEM);
		}
	}
#else
	static uint8_t scratch[CONFIG_TS_VERIFY_SCRATCH];
#endif
	const uint32_t payload_len = v->manifest_len + v->wasm_len;
	const size_t need = 2 * (64 + SIG_STRUCTURE_PREFIX_MAX + (size_t)payload_len);

	if (need > (size_t)CONFIG_TS_VERIFY_SCRATCH) {
		VS_REJECT(TS_E_PARAM); /* 容器超出验签缓冲（板级 TS_VERIFY_SCRATCH 调档） */
	}
	uint8_t *sm = scratch;
	uint8_t *mbuf = scratch + 64 + SIG_STRUCTURE_PREFIX_MAX + payload_len;

	if (read_all(slot, sig_off, sm, 64) != TS_OK) {
		VS_REJECT(TS_E_IO);
	}
	size_t tail = 64 + sig_structure_prefix(sm + 64, payload_len);
	const size_t smlen = tail + payload_len;

	uint32_t coff = payload_data;
	uint32_t roff = v->manifest_off;
	uint32_t remain = payload_len;
	uint8_t raw[512];

	while (remain > 0) {
		uint32_t n = remain > sizeof(raw) ? (uint32_t)sizeof(raw) : remain;

		if (read_all(slot, coff, raw, n) != TS_OK ||
		    read_all(slot, roff, sm + tail, n) != TS_OK) {
			VS_REJECT(TS_E_IO);
		}
		if (memcmp(raw, sm + tail, n) != 0) {
			VS_REJECT(TS_E_INVALID_SIG); /* 内嵌副本 ≠ 裸段（签 A 装 B） */
		}
		coff += n;
		roff += n;
		tail += n;
		remain -= n;
	}

	/* tweetnacl 验签：m 出参为**独立缓冲**（不可与 sm 别名——
	 * crypto_sign_open 在验证前即以 m[32..64)=pk 作暂存，别名会破坏
	 * sm 内签名 S 段〔真机调试实证〕）。多根（EC2）：失败键重试——
	 * crypto_sign_open 只读 sm（出参写 mbuf），重试安全。 */
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
		VS_REJECT(TS_E_INVALID_SIG);
	}
	return TS_OK;
}

/* ---- 信任表更新链（EC2 · Q-43④/DEC-60④；CONFIG_TS_STORE_TRUST）----------- */

#ifdef CONFIG_TS_STORE_TRUST

/* COSE_Sign1 RAM 解析（与 TSAP 容器同构：tag18 + array4 精确子集——
 * phdr bstr(3){1:-8} / uhdr map(0) / payload bstr / sig bstr(64)）。
 * payload/sig 指向入参内部（零拷贝——生存期 = 调用域）。 */
static ts_res_t cose_ram_parse(const uint8_t *cose, uint32_t len,
			       const uint8_t **payload, uint32_t *plen,
			       const uint8_t **sig)
{
	if (cose == NULL || len < 2 + 2 + 3 + 1 + 2 + 2 + 64) {
		return TS_E_INVALID_SIG;
	}
	uint32_t pos = 0;

	if (cose[pos] != 0xd2 || cose[pos + 1] != 0x84) {
		return TS_E_INVALID_SIG;
	}
	pos += 2;
	/* ① protected bstr(3) {0xa1,0x01,0x27}（DR-21 单键纪律——与 TSAP 同源） */
	if (cose[pos] != 0x43) {
		return TS_E_INVALID_SIG;
	}
	static const uint8_t want_phdr[3] = {0xa1, 0x01, 0x27};

	if (memcmp(cose + pos + 1, want_phdr, 3) != 0) {
		return TS_E_INVALID_SIG;
	}
	pos += 4;
	/* ② unprotected map(0) */
	if (cose[pos] != 0xa0) {
		return TS_E_INVALID_SIG;
	}
	pos += 1;
	/* ③ payload bstr：短/1B/2B 头（表 ≤512B 域） */
	uint8_t ib = cose[pos++];
	uint32_t n;

	if ((ib & 0xE0) != 0x40) {
		return TS_E_INVALID_SIG;
	}
	if ((ib & 0x1F) < 24) {
		n = ib & 0x1F;
	} else if ((ib & 0x1F) == 24) {
		n = cose[pos++];
	} else if ((ib & 0x1F) == 25) {
		n = ((uint32_t)cose[pos] << 8) | cose[pos + 1];
		pos += 2;
	} else {
		return TS_E_INVALID_SIG;
	}
	if (n == 0 || pos + n + 2 + 64 > len) {
		return TS_E_INVALID_SIG;
	}
	*payload = cose + pos;
	*plen = n;
	pos += n;
	/* ④ signature bstr(64) */
	if (cose[pos] != 0x58 || cose[pos + 1] != 64) {
		return TS_E_INVALID_SIG;
	}
	*sig = cose + pos + 2;
	return TS_OK;
}

/* 单键校验：sm = sig ‖ Sig_structure 定前缀 ‖ payload（slot 路径同构——
 * sig_structure_prefix 共用）。scratch 双区（sm/m 不别名——同上约束）。 */
static bool cose_ram_check(const uint8_t *payload, uint32_t plen,
			   const uint8_t *sig, const uint8_t pubkey[32])
{
	static uint8_t tsm[64 + 20 + CONFIG_TS_TRUST_TABLE_MAX];
	static uint8_t tmbuf[64 + 20 + CONFIG_TS_TRUST_TABLE_MAX];
	const size_t need = 2 * (64 + 20 + (size_t)plen);

	if (need > sizeof(tsm)) {
		return false; /* 表超出验签缓冲（结构性不可达——TABLE_MAX 域内） */
	}
	memcpy(tsm, sig, 64);
	size_t tail = 64 + sig_structure_prefix(tsm + 64, plen);

	memcpy(tsm + tail, payload, plen);
	unsigned long long mlen = 0;

	return crypto_sign_open(tmbuf, &mlen, tsm,
				(unsigned long long)(tail + plen), pubkey) == 0;
}

ts_res_t ts_appmgr_trust_update(const uint8_t *cose, uint32_t cose_len)
{
	/* ① 结构解析 + payload 表 schema 走查（最廉拒先——无密码学开销） */
	const uint8_t *pl;
	uint32_t plen;
	const uint8_t *sig;

	if (cose_ram_parse(cose, cose_len, &pl, &plen, &sig) != TS_OK) {
		return TS_E_INVALID_SIG;
	}
	ts_trust_table_t cand;

	if (ts_store_trust_validate(pl, plen, &cand) != TS_OK) {
		return TS_E_PARAM; /* 表 schema 违例（生成侧错误） */
	}
	/* ② 单调 ver 严格 N+1（TUF：重放与跳版均拒） */
	const ts_trust_table_t *cur = ts_store_trust();

	if (cand.ver != cur->ver + 1U) {
		printk("[trust] reject ver %u (current %u)\n", cand.ver, cur->ver);
		return TS_E_RANGE;
	}
	/* ③ 签名对当前有效集任一键（现信任键方可推动次版——被撤键无力回天） */
	const uint8_t *set[TS_APPMGR_TRUST_SET_MAX];
	uint8_t n = ts_appmgr_trust_effective(set);
	bool ok = false;

	for (uint8_t i = 0; i < n; i++) {
		if (cose_ram_check(pl, plen, sig, set[i])) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		printk("[trust] reject L%d\n", __LINE__);
		return TS_E_INVALID_SIG;
	}
	/* ④ 持久化（乒乓写 + 回读校验 + RAM 重载）+ 留痕 */
	ts_res_t r = ts_store_trust_write(pl, plen);

	if (r != TS_OK) {
		return r;
	}
	printk("[trust] table ver=%u keys=%u\n", cand.ver, cand.n_keys);
	const ts_trust_evt_t tevt = {.ver = cand.ver, .n_keys = cand.n_keys};
	const ts_evt_t evt = {
		.id = TS_EVT_TRUST_UPDATED, .t_ms = ts_time_ms(),
		.data = &tevt, .len = sizeof(tevt),
	};

	ts_evt_publish(&evt);
	return TS_OK;
}

#endif /* CONFIG_TS_STORE_TRUST */
