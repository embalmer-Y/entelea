/* SPDX-License-Identifier: Apache-2.0 */
/*
 * provisioning 只读访问（LLD-ts-store §4，DEC-30⑤ CBOR schema v1 + CG2 v2）。
 * 合同 10 强制点：**本文件零写调用**（L5 机械检查目标）；写通道仅烧录期
 *（prov_test.c 测试注入 / 外部工具 / Agent deploy_push_prov，MA3）。
 *
 * PROV 分区布局（小端）：u32 cbor_len | u16 crc16(cbor) | cbor[]
 * CBOR schema v1（定稿 2026-09-22，键序固定）：
 *   map{"v":1, "node_id":tstr, "cube_id":tstr, "routers":[tstr×≤4],
 *        "pk0":bstr(32), "pk1":bstr(32), "cred":tstr,
 *        "pwr_ma":uint, "estop":uint}
 * CBOR schema v2（CG2 · Q-42，键序 = v1 前缀 + 尾部追加）：
 *   map{"v":2, …v1 同序九键…, "deploy_mode":uint(0|1), "estop_remote_clear":uint(0|1)}
 * schema v2 尾部追加第三键（F3 · DEC-62①，12 对）：
 *   …, "lconf":uint(0|1)   ——安装闸本地确认（0=off 无头缺省）
 * schema v2 尾部追加 bstr 三元组（P1-2 · DEC-63③/Q-47③，15 对）：
 *   …, "lconf":uint, "tls_ca":bstr(DER≤768), "tls_cert":bstr(DER≤768),
 *       "tls_key":bstr(DER≤256)
 *   ——TLS mTLS 供给（len=0 键缺省形态合法 = TLS off；三键须同现同缺）
 * v1/v2(11对) 兼容缺省（Q-42/DEC-62/DEC-63）：缺字段 = deploy_mode
 * orchestrated + estop_remote_clear enabled + lconf 0 + TLS off（存量
 * prov 零重刷）。解码器为固定 schema 的**确定性子集实现**
 * （definite-length map/text/bytes/uint；任何超集/不定长 = TS_E_IO）
 * ——不引入通用 CBOR 依赖（真机足迹纪律）。
 */
#include <string.h>
#include <ts/store.h>
#include "internal.h"

/* 未装载/装载失败兜底 = v1 等义（estop_remote_clear enabled——测试构建常
 * 无 prov；装载失败生产板走 boot fail-safe，此值仅为失败前的观测安全侧）。 */
static ts_prov_t prov = {
	.deploy_mode = TS_DEPLOY_ORCHESTRATED,
	.estop_remote_clear = 1,
};

/* TLS 三元组装载态（DEC-63③）：独立静态——ts_prov_t 保持小足迹；
 * validate 通道零拷贝不覆写。无 TLS 构建（TS_NET_TLS=n）不付此足迹：
 * 15 对 prov 仍可装载（bstr 仅消费走查，len 镜像照记 = TLS off）。 */
#ifdef CONFIG_TS_NET_TLS
static ts_prov_tls_t prov_tls;
#define PROV_TLS_WALK_OUT (&prov_tls)
#else
#define PROV_TLS_WALK_OUT (NULL)
#endif

/* V0.4.0 M10（Q-34）：schema 走查提取——validate（维护通道写入前校验）
 * 与 load 共用同一条确定性解析路径（单一事实源）。tls_out=NULL =
 * validate 通道（TLS bstr 仅消费零拷贝，不触碰活态）。 */
static ts_res_t prov_walk(const uint8_t *cbor, uint32_t cbor_len,
			  ts_prov_t *out, ts_prov_tls_t *tls_out);

/* ---- 分区读取 + 校验 ------------------------------------------------------ */

ts_res_t ts_store_prov_load(void)
{
	uint8_t hdr[6];

	if (ts_store_backend.read(TS_PART_PROV, 0, hdr, sizeof(hdr)) != TS_OK) {
		return TS_E_IO;
	}
	uint32_t cbor_len = ts_get_le32(hdr);
	uint16_t crc = ts_get_le16(hdr + 4);

	if (cbor_len == 0 || cbor_len > 3800) { /* PROV 分区 4KiB 内合理上限（结构性） */
		return TS_E_IO;
	}
	static uint8_t cbor[3800];

	if (ts_store_backend.read(TS_PART_PROV, sizeof(hdr), cbor, cbor_len) != TS_OK) {
		return TS_E_IO;
	}
	if (ts_crc16(cbor, cbor_len) != crc) {
		return TS_E_IO; /* CRC 失败 → 调用方 fail-safe（合同 6） */
	}

	return prov_walk(cbor, cbor_len, &prov, PROV_TLS_WALK_OUT);
}

/* V0.4.0 M10（Q-34）：schema 走查提取——validate（维护通道写入前校验）
 * 与 load 共用同一条确定性解析路径（单一事实源）。tls_out=NULL =
 * validate 通道（TLS bstr 仅消费零拷贝，不触碰活态）。 */
static ts_res_t prov_walk(const uint8_t *cbor, uint32_t cbor_len,
			  ts_prov_t *out, ts_prov_tls_t *tls_out)
{
	ts_rd_t r = {.p = cbor, .rem = cbor_len};
	uint8_t ib;

	if (!ts_rd_u8(&r, &ib) || (uint8_t)(ib & 0xE0) != 0xA0) {
		return TS_E_IO;
	}
	uint32_t pairs;

	/* v1 = 9 对；v2 = 11 对（CG2）/ 12 对（F3 lconf）/ 15 对（P1-2 TLS
	 * 三元组）——键序 = v1 前缀 + 尾部追加，Q-42 纪律 */
	if (!ts_rd_len(&r, ib & 0x1F, &pairs) ||
	    (pairs != 9 && pairs != 11 && pairs != 12 && pairs != 15)) {
		return TS_E_IO;
	}

	/* v1/v2(11/12) 缺字段缺省（Q-42 现网兼容 + F3 无头缺省 + P1-2 TLS
	 * off——先置再走查，out 可能为脏 scratch） */
	out->deploy_mode = TS_DEPLOY_ORCHESTRATED;
	out->estop_remote_clear = 1;
	out->local_confirm = 0;
	out->tls_ca_len = 0;
	out->tls_cert_len = 0;
	out->tls_key_len = 0;
	char key[20];
	uint32_t u;

	/* 键序固定（定稿 schema）；逐键消费 */
	static const char *const keys[15] = {
		"v", "node_id", "cube_id", "routers", "pk0", "pk1", "cred", "pwr_ma",
		"estop", "deploy_mode", "estop_remote_clear", "lconf",
		"tls_ca", "tls_cert", "tls_key",
	};

	for (uint32_t k = 0; k < pairs; k++) {
		if (!ts_rd_tstr(&r, key, sizeof(key)) || strcmp(key, keys[k]) != 0) {
			return TS_E_IO;
		}
		switch (k) {
		case 0: /* v：9 对 ⇔ v1、11/12/15 对 ⇔ v2（错配 = 结构损坏） */
			if (!ts_rd_uint(&r, &u) || u != (uint32_t)(pairs == 9 ? 1 : 2)) {
				return TS_E_IO;
			}
			break;
		case 1:
			if (!ts_rd_tstr(&r, out->node_id, sizeof(out->node_id))) {
				return TS_E_IO;
			}
			break;
		case 2:
			if (!ts_rd_tstr(&r, out->cube_id, sizeof(out->cube_id))) {
				return TS_E_IO;
			}
			break;
		case 3: { /* routers: definite array of tstr ≤4 */
			uint8_t ab;

			if (!ts_rd_u8(&r, &ab) || (uint8_t)(ab & 0xE0) != 0x80) {
				return TS_E_IO;
			}
			uint32_t n;

			if (!ts_rd_len(&r, ab & 0x1F, &n) || n > 4) {
				return TS_E_IO;
			}
			for (uint32_t i = 0; i < n; i++) {
				if (!ts_rd_tstr(&r, out->router_locators[i],
					     sizeof(out->router_locators[i]))) {
					return TS_E_IO;
				}
			}
			for (uint32_t i = n; i < 4; i++) {
				out->router_locators[i][0] = '\0';
			}
			break;
		}
		case 4:
			if (!ts_rd_bstr(&r, out->root_pubkeys[0], 32)) {
				return TS_E_IO;
			}
			break;
		case 5:
			if (!ts_rd_bstr(&r, out->root_pubkeys[1], 32)) {
				return TS_E_IO;
			}
			break;
		case 6:
			if (!ts_rd_tstr(&r, out->zenoh_cred, sizeof(out->zenoh_cred))) {
				return TS_E_IO;
			}
			break;
		case 7:
			if (!ts_rd_uint(&r, &out->power_budget_ma)) {
				return TS_E_IO;
			}
			break;
		case 8:
			if (!ts_rd_uint(&r, &u) || u > 0xFFU) {
				return TS_E_IO; /* estop 标志域 u8（防静默截断，IR-10） */
			}
			out->estop_trigger_flags = (uint8_t)u;
			break;
		case 9: /* deploy_mode：域外值 fail-closed（未知模式不静默归并） */
			if (!ts_rd_uint(&r, &u) || u > (uint32_t)TS_DEPLOY_STANDALONE) {
				return TS_E_IO;
			}
			out->deploy_mode = (uint8_t)u;
			break;
		case 10: /* estop_remote_clear：0|1 之外 fail-closed（12 对形态） */
			if (!ts_rd_uint(&r, &u) || u > 1U) {
				return TS_E_IO;
			}
			out->estop_remote_clear = (uint8_t)u;
			break;
		default:
			if (k == 11) { /* lconf（F3 · DEC-62①）：本地确认；域外 fail-closed */
				if (!ts_rd_uint(&r, &u) || u > 1U) {
					return TS_E_IO;
				}
				out->local_confirm = (uint8_t)u;
				break;
			}
			/* 12/13/14：TLS 三元组（P1-2 · DEC-63③）——DER bstr；
			 * tls_out=NULL = validate 通道零拷贝走查 */
			uint16_t blen = 0;
			uint8_t *dst = NULL;
			size_t cap = 0;
			uint16_t *dlen = NULL;
			uint16_t *tls_dlen = NULL;

			switch (k) {
			case 12:
				dst = tls_out ? tls_out->ca : NULL;
				cap = TS_PROV_TLS_CA_MAX;
				dlen = &out->tls_ca_len;
				tls_dlen = tls_out ? &tls_out->ca_len : NULL;
				break;
			case 13:
				dst = tls_out ? tls_out->cert : NULL;
				cap = TS_PROV_TLS_CERT_MAX;
				dlen = &out->tls_cert_len;
				tls_dlen = tls_out ? &tls_out->cert_len : NULL;
				break;
			default:
				dst = tls_out ? tls_out->key : NULL;
				cap = TS_PROV_TLS_KEY_MAX;
				dlen = &out->tls_key_len;
				tls_dlen = tls_out ? &tls_out->key_len : NULL;
				break;
			}
			if (!ts_rd_bstr_len(&r, dst, cap, &blen) || blen == 0) {
				return TS_E_IO; /* 键在而值空 = 结构损坏（fail-closed） */
			}
			*dlen = blen;
			if (tls_dlen != NULL) {
				*tls_dlen = blen; /* ts_prov_tls_t 自身长度镜像（消费者单一面） */
			}
			break;
		}
	}
	return TS_OK;
}

ts_res_t ts_store_prov_validate(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len > 3800) {
		return TS_E_PARAM;
	}
	ts_prov_t scratch;

	/* TLS 三元组零拷贝（tls_out=NULL）：validate 不触碰装载活态（DEC-63③） */
	return prov_walk(cbor, len, &scratch, NULL);
}

const ts_prov_t *ts_store_prov(void)
{
	return &prov;
}

#ifdef CONFIG_TS_NET_TLS
const ts_prov_tls_t *ts_store_prov_tls(void)
{
	return &prov_tls;
}
#endif
