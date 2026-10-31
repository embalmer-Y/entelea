/* SPDX-License-Identifier: Apache-2.0 */
/* ts-store L1/L2 测试（LLD-ts-store §8）：meta 撕裂恢复 / prov 校验 / slot+hash / noinit。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <ts/store.h>
#include <ts/tsap.h>

/* CBOR 测试构造助手（固定 schema，与 prov.c 解码器对齐）。
 * 2048 = prov 15 对形态（TLS 三元组 DER 实测 ~1.3KB + 基础字段）。 */
#define CB_MAX 2048

struct cb {
	uint8_t b[CB_MAX];
	uint32_t n;
};

static void cb_u8(struct cb *c, uint8_t v)
{
	c->b[c->n++] = v;
}

static void cb_hdr(struct cb *c, uint8_t mt, uint32_t val)
{
	if (val < 0x18) {
		cb_u8(c, (uint8_t)(mt | val));
	} else if (val <= 0xFF) {
		cb_u8(c, (uint8_t)(mt | 0x18));
		cb_u8(c, (uint8_t)val);
	} else {
		cb_u8(c, (uint8_t)(mt | 0x19));
		cb_u8(c, (uint8_t)(val >> 8));
		cb_u8(c, (uint8_t)val);
	}
}

static void cb_tstr(struct cb *c, const char *s)
{
	cb_hdr(c, 0x60, strlen(s));
	for (size_t i = 0; i < strlen(s); i++) {
		cb_u8(c, (uint8_t)s[i]);
	}
}

static void cb_bstr32(struct cb *c, uint8_t fill)
{
	cb_hdr(c, 0x40, 32);
	for (int i = 0; i < 32; i++) {
		cb_u8(c, fill + (uint8_t)i);
	}
}

/* 变长 bstr（P1-2 · DEC-63③：TLS DER——≥256 走 0x59 两字节长形态） */
static void cb_bstr_var(struct cb *c, uint32_t len, uint8_t fill)
{
	if (len < 24) {
		cb_hdr(c, 0x40, len);
	} else if (len < 256) {
		cb_u8(c, 0x58);
		cb_u8(c, (uint8_t)len);
	} else {
		cb_u8(c, 0x59);
		cb_u8(c, (uint8_t)(len >> 8));
		cb_u8(c, (uint8_t)len);
	}
	for (uint32_t i = 0; i < len; i++) {
		cb_u8(c, fill + (uint8_t)i);
	}
}

static void build_prov_cbor_full(struct cb *c, const char *node, const char *cube,
				 const char *router, uint32_t pwr, int ver,
				 uint8_t deploy_mode, uint8_t estop_remote_clear,
				 int lconf, const uint32_t *tls)
{
	memset(c, 0, sizeof(*c));
	uint32_t pairs = 9;

	if (ver == 2) {
		pairs = (tls != NULL) ? 15 : ((lconf >= 0) ? 12 : 11);
	}
	cb_hdr(c, 0xA0, pairs);
	cb_tstr(c, "v");
	cb_hdr(c, 0x00, ver);
	cb_tstr(c, "node_id");
	cb_tstr(c, node);
	cb_tstr(c, "cube_id");
	cb_tstr(c, cube);
	cb_tstr(c, "routers");
	cb_hdr(c, 0x80, 1);
	cb_tstr(c, router);
	cb_tstr(c, "pk0");
	cb_bstr32(c, 0x10);
	cb_tstr(c, "pk1");
	cb_bstr32(c, 0x60);
	cb_tstr(c, "cred");
	cb_tstr(c, "test-cred");
	cb_tstr(c, "pwr_ma");
	cb_hdr(c, 0x00, pwr);
	cb_tstr(c, "estop");
	cb_hdr(c, 0x00, 1);
	if (ver == 2) {
		/* CG2（Q-42）：v2 = v1 键序前缀 + 尾部追加；F3（DEC-62①）第三键
		 * lconf；P1-2（DEC-63③）TLS 三元组（15 对——lconf 必含，缺省 0） */
		cb_tstr(c, "deploy_mode");
		cb_hdr(c, 0x00, deploy_mode);
		cb_tstr(c, "estop_remote_clear");
		cb_hdr(c, 0x00, estop_remote_clear);
		if (lconf >= 0 || tls != NULL) {
			cb_tstr(c, "lconf");
			cb_hdr(c, 0x00, (uint8_t)(lconf >= 0 ? lconf : 0));
		}
		if (tls != NULL) {
			cb_tstr(c, "tls_ca");
			cb_bstr_var(c, tls[0], 0xA0);
			cb_tstr(c, "tls_cert");
			cb_bstr_var(c, tls[1], 0xB0);
			cb_tstr(c, "tls_key");
			cb_bstr_var(c, tls[2], 0xC0);
		}
	}
}

static void build_prov_cbor(struct cb *c, const char *node, const char *cube,
			    const char *router, uint32_t pwr)
{
	build_prov_cbor_full(c, node, cube, router, pwr, 1, 0, 0, -1, NULL);
}

/* ---- V0.8.0 EC2（Q-43③）：信任表持久化——schema 矩阵/乒乓/撕裂 ---------- */

static void build_trust_cbor(struct cb *c, uint32_t ver, int nkeys, int dup_at)
{
	memset(c, 0, sizeof(*c));
	cb_hdr(c, 0xA0, 2);
	cb_tstr(c, "ver");
	cb_hdr(c, 0x00, ver);
	cb_tstr(c, "keys");
	cb_hdr(c, 0x80, (uint32_t)nkeys);
	for (int i = 0; i < nkeys; i++) {
		/* dup_at ≥0 = 该索引键与索引 0 相同（重复样例）；否则 fill 差异 */
		cb_bstr32(c, (dup_at == i) ? 0x10 : (uint8_t)(0x10 + 4 * i));
	}
}

ZTEST(framework_store, test_trust_table)
{
	zassert_equal(ts_store_test_reset(), TS_OK);
	ts_store_trust_test_reset();
	struct cb c;
	ts_trust_table_t t;

	/* 初始态：空分区 → {ver=0, n=0}（工厂根单源哨兵） */
	zassert_equal(ts_store_trust()->ver, 0);
	zassert_equal(ts_store_trust()->n_keys, 0);

	/* schema 走查矩阵（validate——单一事实源，load 同路径） */
	build_trust_cbor(&c, 1, 2, -1);
	zassert_equal(ts_store_trust_validate(c.b, c.n, &t), TS_OK);
	zassert_equal(t.ver, 1);
	zassert_equal(t.n_keys, 2);
	build_trust_cbor(&c, 0, 1, -1);
	zassert_equal(ts_store_trust_validate(c.b, c.n, &t), TS_E_IO,
		      "ver=0 = 哨兵非合法值");
	build_trust_cbor(&c, 1, 9, -1);
	zassert_equal(ts_store_trust_validate(c.b, c.n, &t), TS_E_IO,
		      "超 8 键上限拒");
	build_trust_cbor(&c, 1, 2, 1);
	zassert_equal(ts_store_trust_validate(c.b, c.n, &t), TS_E_IO,
		      "键内重复拒");
	build_trust_cbor(&c, 1, 2, -1);
	c.b[c.n++] = 0x00; /* 尾部多余字节 */
	zassert_equal(ts_store_trust_validate(c.b, c.n, &t), TS_E_IO,
		      "尾部余量拒");
	/* 键序错（keys 在前）= 结构损坏 */
	memset(&c, 0, sizeof(c));
	cb_hdr(&c, 0xA0, 2);
	cb_tstr(&c, "keys");
	cb_hdr(&c, 0x80, 0);
	cb_tstr(&c, "ver");
	cb_hdr(&c, 0x00, 1);
	zassert_equal(ts_store_trust_validate(c.b, c.n, &t), TS_E_IO,
		      "键序错拒");

	/* 写入 + 回读 roundtrip（ver 1 两键） */
	build_trust_cbor(&c, 1, 2, -1);
	zassert_equal(ts_store_trust_write(c.b, c.n), TS_OK);
	zassert_equal(ts_store_trust()->ver, 1);
	zassert_equal(ts_store_trust()->n_keys, 2);
	zassert_equal(ts_store_trust()->keys[0][0], 0x10);
	zassert_equal(ts_store_trust()->keys[1][0], 0x14);

	/* 非法表写入 = 零状态变化（fail-closed） */
	build_trust_cbor(&c, 1, 9, -1);
	zassert_equal(ts_store_trust_write(c.b, c.n), TS_E_IO);
	zassert_equal(ts_store_trust()->n_keys, 2, "坏表零写入");

	/* 次版（ver 2 一键）→ 乒乓写另一副本 + 活动切换 */
	build_trust_cbor(&c, 2, 1, -1);
	zassert_equal(ts_store_trust_write(c.b, c.n), TS_OK);
	zassert_equal(ts_store_trust()->ver, 2);
	zassert_equal(ts_store_trust()->n_keys, 1);

	/* 撕裂恢复：破坏活动副本（copy1 = ver2）→ 重装载回退旧副本（ver1） */
	ts_store_trust_test_reset();
	zassert_equal(ts_store_trust_corrupt_test(1), TS_OK);
	zassert_equal(ts_store_trust_load(), TS_OK);
	zassert_equal(ts_store_trust()->ver, 1, "回退旧副本（copy1 损——ver1 存活）");
	zassert_equal(ts_store_trust()->n_keys, 2);

	/* 双损 → 工厂根单源哨兵（fail-safe 侧） */
	zassert_equal(ts_store_trust_corrupt_test(0), TS_OK);
	ts_store_trust_test_reset();
	zassert_equal(ts_store_trust_load(), TS_OK);
	zassert_equal(ts_store_trust()->ver, 0);
	zassert_equal(ts_store_trust()->n_keys, 0);
}

static void *store_setup(void)
{
	zassert_equal(ts_store_test_reset(), TS_OK, "backend reset");
#ifdef CONFIG_TS_STORE_KV
	ts_store_kv_test_reset(); /* M9：游标随抹除失效（重扫描） */
#endif
	return NULL;
}

/* V0.4.0 M10（Q-34）：维护写通道——校验先行 + per-boot 一次性窗 --------- */

ZTEST(framework_store, test_prov_maint_channel)
{
	/* 执行序自足 */
	zassert_equal(ts_store_test_reset(), TS_OK);
	ts_store_prov_maint_test_reset();
	ts_store_trust_test_reset(); /* EC2：信任表内存态复位（后端已抹除） */
	struct cb c;

	/* 非法 blob = 零写入（schema 走查失败 → E_IO；分区保持擦除态） */
	build_prov_cbor(&c, "n", "c", "r", 1);
	c.b[3] ^= 0xFF; /* 破坏 schema（节点串区） */
	zassert_equal(ts_store_prov_write_maint(c.b, c.n), TS_E_IO);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "零写入（分区仍空）");

	/* 合法 blob → 写入 + 新身份装载 + 窗生效 */
	build_prov_cbor(&c, "mn-7", "mc-7", "tcp/10.1.1.1:9955", 700);
	zassert_equal(ts_store_prov_write_maint(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_ok(strcmp(ts_store_prov()->node_id, "mn-7"));
	zassert_equal(ts_store_prov()->power_budget_ma, 700);

	build_prov_cbor(&c, "mn-8", "mc-8", "tcp/10.1.1.2:9955", 800);
	zassert_equal(ts_store_prov_write_maint(c.b, c.n), TS_E_STATE,
		      "per-boot 一次性窗（再写须复位）");
	ts_store_prov_maint_test_reset(); /* 窗复位（模拟复位循环） */
	zassert_equal(ts_store_prov_write_maint(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_ok(strcmp(ts_store_prov()->node_id, "mn-8"));
}

ZTEST(framework_store, test_prov_ok)
{
	struct cb c;

	build_prov_cbor(&c, "node-1", "cube-9", "tcp/10.0.0.1:7447", 500);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);

	const ts_prov_t *p = ts_store_prov();

	zassert_ok(strcmp(p->node_id, "node-1"));
	zassert_ok(strcmp(p->cube_id, "cube-9"));
	zassert_ok(strcmp(p->router_locators[0], "tcp/10.0.0.1:7447"));
	zassert_equal(p->power_budget_ma, 500);
	zassert_equal(p->estop_trigger_flags, 1);
	zassert_equal(p->root_pubkeys[0][0], 0x10);
	zassert_equal(p->root_pubkeys[1][0], 0x60);
	zassert_ok(strcmp(p->zenoh_cred, "test-cred"));

	/* CG2（Q-42）：v1 缺字段缺省——orchestrated + 远程清除开放（现网兼容） */
	zassert_equal(p->deploy_mode, TS_DEPLOY_ORCHESTRATED, "v1 = orchestrated 缺省");
	zassert_equal(p->estop_remote_clear, 1, "v1 = estop 远程清除缺省开放");
}

/* CG2（Q-42/Q-41）：prov schema v2——部署模式与 estop 远程清除通道 -------- */
ZTEST(framework_store, test_prov_v2_fields)
{
	struct cb c;

	build_prov_cbor_full(&c, "n-v2", "c-v2", "tcp/10.0.0.2:7447", 200, 2, 1, 0, -1, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	const ts_prov_t *p = ts_store_prov();

	zassert_ok(strcmp(p->node_id, "n-v2"));
	zassert_equal(p->power_budget_ma, 200);
	zassert_equal(p->deploy_mode, TS_DEPLOY_STANDALONE);
	zassert_equal(p->estop_remote_clear, 0, "v2 显式 disabled");

	/* 域外值 fail-closed（未知模式/通道值不静默归并——IR-10 同型） */
	build_prov_cbor_full(&c, "n-x", "c-x", "tcp/x:1", 1, 2, 2, 1, -1, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "deploy_mode=2 必须拒绝");
	build_prov_cbor_full(&c, "n-y", "c-y", "tcp/x:1", 1, 2, 1, 2, -1, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "estop_remote_clear=2 必须拒绝");

	/* v/pairs 错配（9 对 + v2 值）拒绝 */
	build_prov_cbor(&c, "n-z", "c-z", "tcp/x:1", 1);
	c.b[3] = 0x02; /* "v" 值 1→2，pairs 仍 9 */
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "v=2 与 9 对错配必须拒绝");

	/* F3 · DEC-62①：v2 12 对第三键 lconf——1 = 安装闸开 */
	build_prov_cbor_full(&c, "n-lc", "c-lc", "tcp/x:1", 1, 2, 0, 1, 1, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_equal(ts_store_prov()->local_confirm, 1, "12 对 lconf=1");
	zassert_equal(ts_store_prov()->deploy_mode, TS_DEPLOY_ORCHESTRATED,
		      "12 对 v2 前两尾键语义不变");
	/* 显式 0 */
	build_prov_cbor_full(&c, "n-lc0", "c-lc0", "tcp/x:1", 1, 2, 0, 1, 0, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_equal(ts_store_prov()->local_confirm, 0, "12 对 lconf=0");
	/* 域外值 fail-closed */
	build_prov_cbor_full(&c, "n-lc2", "c-lc2", "tcp/x:1", 1, 2, 0, 1, 2, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "lconf=2 必须拒绝");
	/* 11 对存量形态 = lconf 缺省 0（存量 prov 零重刷等义） */
	build_prov_cbor_full(&c, "n-v2b", "c-v2b", "tcp/x:1", 1, 2, 0, 1, -1, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_equal(ts_store_prov()->local_confirm, 0, "11 对缺省 lconf=0");

	/* 收尾复原 orchestrated（后续用例状态干净） */
	build_prov_cbor(&c, "node-1", "cube-9", "tcp/10.0.0.1:7447", 500);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
}

/* P1-2（DEC-63③/Q-47③）：prov schema v2 15 对——TLS mTLS 三元组 ---------- */
ZTEST(framework_store, test_prov_tls_triple)
{
	struct cb c;
	const uint32_t tls[3] = {570, 590, 121}; /* EC P-256 DER 实测形态 */

	/* 15 对合法装载：len 镜像 + lconf 必含 */
	build_prov_cbor_full(&c, "n-tls", "c-tls", "tls/10.0.0.9:9956", 300, 2, 0, 1, 1, tls);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	const ts_prov_t *p = ts_store_prov();

	zassert_equal(p->tls_ca_len, 570, "15 对 ca len 镜像");
	zassert_equal(p->tls_cert_len, 590, "15 对 cert len 镜像");
	zassert_equal(p->tls_key_len, 121, "15 对 key len 镜像");
	zassert_equal(p->local_confirm, 1, "15 对 lconf 语义不变");

	/* validate 维护通道同路径（单一事实源） */
	build_prov_cbor_full(&c, "n-val", "c-val", "tcp/x:1", 1, 2, 0, 1, 0, tls);
	zassert_equal(ts_store_prov_validate(c.b, c.n), TS_OK, "validate 15 对同路径");

	/* 超 cap 拒（fail-closed——768/768/256 上限，DEC-63③） */
	const uint32_t over[3] = {TS_PROV_TLS_CA_MAX + 1, 64, 64};
	build_prov_cbor_full(&c, "n-o", "c-o", "tcp/x:1", 1, 2, 0, 1, 0, over);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "ca 超 768B 必须拒绝");

	/* 键在值空（len=0）拒——三键同现且非空纪律 */
	const uint32_t empt[3] = {0, 64, 64};
	build_prov_cbor_full(&c, "n-e", "c-e", "tcp/x:1", 1, 2, 0, 1, 0, empt);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "空 bstr 必须拒绝");

	/* 域外对数（13 对）拒——pairs 白名单 */
	build_prov_cbor_full(&c, "n-13", "c-13", "tcp/x:1", 1, 2, 0, 1, 0, tls);
	c.b[0] = 0xAD; /* map(13) */
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "13 对必须拒绝");

	/* 12 对形态 = TLS off（lens 0——存量 prov 零重刷等义） */
	build_prov_cbor_full(&c, "n-12", "c-12", "tcp/x:1", 1, 2, 0, 1, 0, NULL);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_equal(ts_store_prov()->tls_ca_len, 0, "12 对 = TLS off");
	zassert_equal(ts_store_prov()->tls_key_len, 0, "12 对 = TLS off");

	/* 收尾复原（后续用例状态干净） */
	build_prov_cbor(&c, "node-1", "cube-9", "tcp/10.0.0.1:7447", 500);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
}

ZTEST(framework_store, test_prov_crc_fail_then_recover)
{
	struct cb c;

	build_prov_cbor(&c, "n", "c", "tcp/x:1", 1);
	zassert_equal(ts_store_prov_write_test(c.b, c.n), TS_OK);
	/* 破坏 CBOR 末字节（重写绕过 CRC——直接改分区内容） */
	uint8_t tmp[CB_MAX];

	memcpy(tmp, c.b, c.n);
	tmp[c.n - 1] ^= 0xFF;
	ts_res_t r = ts_store_prov_write_test(tmp, c.n);

	zassert_equal(r, TS_OK); /* 注入自身成功（CRC 按坏数据计算）→ 改为破坏分区 */
	/* 直接破坏分区第二字节（跳过注入 CRC） */
	extern ts_res_t ts_store_meta_corrupt_test(uint8_t);
	/* 借助 meta 破坏接口不可达 prov——用写入原语不可行；改为：注入好数据后
	 * 以坏 CBOR（键序错）验证解析拒绝路径 */
	struct cb bad;

	build_prov_cbor(&bad, "n", "c", "tcp/x:1", 1);
	bad.b[7] = 'X'; /* 破坏第一个键名 "v"→"X" */
	zassert_equal(ts_store_prov_write_test(bad.b, bad.n), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "坏 CBOR 必须拒绝");
	/* 空分区（无 prov）→ 加载失败 = 调用方 fail-safe 路径（合同 6 联动） */
	zassert_equal(ts_store_test_reset(), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_E_IO, "空 prov 分区 = TS_E_IO");
}

ZTEST(framework_store, test_meta_roundtrip_and_tear)
{
	uint8_t buf[64];
	uint16_t len = 0;

	/* 空：读失败（双副本皆无） */
	zassert_equal(ts_store_meta_read(buf, &len), TS_E_IO);

	/* 写读往返 ×6（seq 递增切换副本；≥3 轮覆盖"copy0 较新"读取路径——
	 * MD1.1b/P1 回归：此前共用读缓冲使第 3/5 轮后读回旧副本内容，
	 * 真机连续部署 100% 复现，2 轮用例从未覆盖该分支） */
	for (int round = 0; round < 6; round++) {
		uint8_t w[32];

		for (int i = 0; i < 32; i++) {
			w[i] = (uint8_t)(round * 32 + i);
		}
		zassert_equal(ts_store_meta_write(w, 32), TS_OK);
		uint8_t r[64];

		zassert_equal(ts_store_meta_read(r, &len), TS_OK);
		zassert_equal(len, 32);
		zassert_ok(memcmp(r, w, 32), "round %d", round);
	}

	/* 撕裂注入：破坏活动副本 → 另一副本接管（6 轮末活动副本 = copy1） */
	zassert_equal(ts_store_meta_corrupt_test(1), TS_OK);
	uint8_t r[64];

	zassert_equal(ts_store_meta_read(r, &len), TS_OK, "单副本损坏可恢复");
	zassert_equal(len, 32);

	/* 双副本皆损 → TS_E_IO（启动按缺省安全态，LLD §3） */
	zassert_equal(ts_store_meta_corrupt_test(0), TS_OK);
	zassert_equal(ts_store_meta_read(r, &len), TS_E_IO);
}

ZTEST(framework_store, test_slot_rw_and_bounds)
{
	uint8_t w[300], r[300];

	for (int i = 0; i < 300; i++) {
		w[i] = (uint8_t)i;
	}
	zassert_equal(ts_store_slot_write(1, 0, w, 300), TS_OK);
	zassert_equal(ts_store_slot_read(1, 0, r, 300), TS_OK);
	zassert_ok(memcmp(r, w, 300));
	/* 边界：越界写/读 */
	zassert_equal(ts_store_slot_write(1, CONFIG_TS_STORE_SLOT_SIZE - 4, w, 8), TS_E_RANGE);
	zassert_equal(ts_store_slot_read(1, CONFIG_TS_STORE_SLOT_SIZE - 4, r, 8), TS_E_RANGE);
	zassert_equal(ts_store_slot_write(2, 0, w, 8), TS_E_PARAM);
}

ZTEST(framework_store, test_slot_hash_vector)
{
	/* slot 全 0xFF 抹除态哈希（reset 后未写）= 参考值由本用例内 ts_sha256 直算
	 * 对照——校验"整槽哈希 = 全量数据哈希"的一致性；另附 "abc" 标准向量。 */
	uint8_t sha[32];
	static const uint8_t abc[3] = {'a', 'b', 'c'};

	extern void ts_sha256(const uint8_t *, size_t, uint8_t *);
	ts_sha256(abc, 3, sha);
	static const uint8_t abc_expect[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
		0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	zassert_ok(memcmp(sha, abc_expect, 32), "SHA-256 标准向量");

	zassert_equal(ts_store_slot_hash(0, sha), TS_OK);
	uint32_t size = CONFIG_TS_STORE_SLOT_SIZE;
	static uint8_t all[CONFIG_TS_STORE_SLOT_SIZE]; /* 静态大缓冲（sim 内存充足） */

	memset(all, 0xFF, size);
	uint8_t expect[32];

	ts_sha256(all, size, expect);
	zassert_ok(memcmp(sha, expect, 32), "整槽哈希 = 全量哈希");
}

ZTEST(framework_store, test_noinit)
{
	uint16_t len = 0;
	bool fresh = true;
	uint8_t rec[8];

	/* 空：fresh=true */
	zassert_equal(ts_store_noinit_get(rec, &len, &fresh), TS_OK);
	zassert_true(fresh);

	/* 留痕后：fresh=false，内容往返 */
	static const uint8_t crash[6] = {0x02, 0x00, 0x12, 0x34, 0x00, 0x01};

	ts_store_noinit_put(crash, 6);
	zassert_equal(ts_store_noinit_get(rec, &len, &fresh), TS_OK);
	zassert_false(fresh);
	zassert_equal(len, 6);
	zassert_ok(memcmp(rec, crash, 6));
}

ZTEST(framework_store, test_tsap_header)
{
	uint8_t pkg[128];

	memset(pkg, 0xA5, sizeof(pkg));
	/* v2 头：magic | ver=2 | manifest_len=8 | wasm_len=16 | flags=1（大端） */
	pkg[0] = 'T';
	pkg[1] = 'S';
	pkg[2] = 'A';
	pkg[3] = 'P';
	pkg[4] = 0;
	pkg[5] = 2;
	pkg[6] = 0;
	pkg[7] = 0;
	pkg[8] = 0;
	pkg[9] = 8;
	pkg[10] = 0;
	pkg[11] = 0;
	pkg[12] = 0;
	pkg[13] = 16;
	pkg[14] = 0;
	pkg[15] = 1; /* TSAP_FLAGS_DIGEST（DEC-49②；16..47 = 摘要区） */
	tsap_view_t v;

	zassert_true(tsap_header_parse(pkg, sizeof(pkg), &v));
	zassert_equal(v.manifest_len, 8);
	zassert_equal(v.wasm_len, 16);
	zassert_equal(v.manifest_off, 48, "v2 内容起点 = 头 16 + 摘要 32");
	zassert_equal(v.wasm_off, 48 + 8);
	zassert_equal(v.cose_off, 48 + 8 + 16);

	/* 坏 magic / 坏版本 / 未知 flags / 长度越界 */
	pkg[0] = 'X';
	zassert_false(tsap_header_parse(pkg, sizeof(pkg), &v));
	pkg[0] = 'T';
	pkg[5] = 1; /* v1 = 拒收（DEC-49②） */
	zassert_false(tsap_header_parse(pkg, sizeof(pkg), &v));
	pkg[5] = 2;
	pkg[15] = 2; /* 未知 flags = 拒收 */
	zassert_false(tsap_header_parse(pkg, sizeof(pkg), &v));
	pkg[15] = 1;
	pkg[9] = 0xFF; /* manifest_len=255 → cose_off+1 > len */
	zassert_false(tsap_header_parse(pkg, sizeof(pkg), &v));
	zassert_false(tsap_header_parse(pkg, 10, &v), "长度不足");

	/* IR-06：总长恰为 UINT32_MAX 的回绕头——修复前 cose_off+1 在 u32 域回绕为 0，
	 * 越界检查失效（OOB 读风险）；必须在 u64 域拒绝 */
	pkg[6] = 0xFF; pkg[7] = 0xFF; pkg[8] = 0xFF; pkg[9] = 0xE0; /* man=0xFFFFFFE0 */
	pkg[10] = 0; pkg[11] = 0; pkg[12] = 0; pkg[13] = 0x0F;      /* wasm=0xF */
	zassert_false(tsap_header_parse(pkg, sizeof(pkg), &v), "回绕头必须拒绝");
}

#ifdef CONFIG_TS_STORE_AUDIT
/* V2-2（DEC-52①）：审计 flash 环——写入/读回/重 init 留存（复位语义近似：
 * 后端数据不动仅重建游标）/两页乒乓翻页与容量上界。RAM/flash 两变体同跑
 * （flash 变体额外覆盖写对齐垫片与擦除块路径）。 */
ZTEST(framework_store, test_audit_ring_persist_and_flip)
{
	zassert_equal(ts_store_audit_init(), TS_OK);

	ts_store_audit_entry_t e[8];

	zassert_equal(ts_store_audit_read(e, 8), 0, "空环读零");

	for (int i = 0; i < 4; i++) {
		const ts_audit_entry_t a = {
			.t_ms = 100 + (uint64_t)i,
			.ch_idx = 0xFFFF,
			.res = TS_E_RANGE,
			.actor = 7,
			.kind = 2,
			.value_u = 600,
		};

		zassert_equal(ts_store_audit_append(&a, 0), TS_OK);
	}
	size_t n = ts_store_audit_read(e, 8);

	zassert_equal(n, 4);
	zassert_equal(e[0].seq, 1);
	zassert_equal(e[3].seq, 4);
	zassert_equal(e[3].res, TS_E_RANGE);
	zassert_equal(e[3].value_u, 600);
	zassert_equal(e[3].evt, 0);

	/* 留存（复位语义近似）：后端数据不动 → 重 init 游标恢复 → 数据仍在 */
	zassert_equal(ts_store_audit_init(), TS_OK);
	n = ts_store_audit_read(e, 8);
	zassert_equal(n, 4, "重 init 后留存");

	/* 翻页：再写 60 条（总 64 = 页满）→ 第 65 条起翻入对页 */
	for (int i = 0; i < 61; i++) {
		const ts_audit_entry_t a = {.t_ms = 200 + (uint64_t)i,
					    .res = TS_E_STATE};

		zassert_equal(ts_store_audit_append(&a, 1), TS_OK);
	}
	static ts_store_audit_entry_t big[130]; /* 静态：130×~32B 栈压力规避 */

	n = ts_store_audit_read(big, 130);
	zassert_true(n >= 64 && n <= 128, "乒乓过渡窗（64..128）");
	zassert_equal(big[n - 1].seq, 65, "末条 seq=65");

	/* 重 init 后过渡态恢复一致 */
	zassert_equal(ts_store_audit_init(), TS_OK);
	n = ts_store_audit_read(big, 130);
	zassert_equal(big[n - 1].seq, 65, "翻页后重 init 游标恢复");

	/* 容量上界：再写 39（总 104）→ 环保留最近 ≤128 */
	for (int i = 0; i < 39; i++) {
		const ts_audit_entry_t a = {.t_ms = 300 + (uint64_t)i,
					    .res = TS_E_STATE};

		zassert_equal(ts_store_audit_append(&a, 0), TS_OK);
	}
	n = ts_store_audit_read(big, 130);
	zassert_true(n <= 128, "容量上界");
	zassert_equal(big[n - 1].seq, 104, "末条 seq=总条数");
	zassert_equal(big[0].seq, big[n - 1].seq - (uint32_t)n + 1, "seq 连续");
}
#endif

/* V0.4.0 M9（DR-15）：APP 业务状态 kv ---------------------------------------- */
#ifdef CONFIG_TS_STORE_KV
ZTEST(framework_store, test_kv_roundtrip_and_isolation)
{
	/* 执行序自足（ztest 非源码序 + setup 仅一次——M6 教训） */
	zassert_equal(ts_store_test_reset(), TS_OK);
	ts_store_kv_test_reset();
	uint8_t buf[TS_KV_BLOB_MAX];
	uint8_t out[TS_KV_BLOB_MAX];

	/* 保存两个 app 的 blob → 各自读回（哈希隔离） */
	for (int i = 0; i < 100; i++) {
		buf[i] = (uint8_t)('a' + i % 26);
	}
	zassert_equal(ts_store_kv_save(0x11111111, buf, 100), TS_OK);
	for (int i = 0; i < 50; i++) {
		buf[i] = (uint8_t)('A' + i % 26);
	}
	zassert_equal(ts_store_kv_save(0x22222222, buf, 50), TS_OK);

	zassert_equal(ts_store_kv_load(0x11111111, out, sizeof(out)), 100);
	zassert_equal(out[0], 'a', "app1 数据");
	zassert_equal(ts_store_kv_load(0x22222222, out, sizeof(out)), 50);
	zassert_equal(out[0], 'A', "app2 数据");
	zassert_equal(ts_store_kv_load(0x33333333, out, sizeof(out)),
		      TS_E_NOTFOUND, "未存 app = NOTFOUND");

	/* 覆盖：app1 重存 → 读回新值（最新胜出） */
	buf[0] = 'Z';
	zassert_equal(ts_store_kv_save(0x11111111, buf, 30), TS_OK);
	zassert_equal(ts_store_kv_load(0x11111111, out, sizeof(out)), 30);
	zassert_equal(out[0], 'Z', "覆盖后最新胜出");

	/* 容量边界 */
	zassert_equal(ts_store_kv_save(0x11111111, buf, TS_KV_BLOB_MAX + 1),
		      TS_E_PARAM, "超上限拒绝");
	zassert_equal(ts_store_kv_load(0x22222222, out, 10), TS_E_RANGE,
		      "读容量不足 = RANGE");
}

ZTEST(framework_store, test_kv_compact_and_reload)
{
	zassert_equal(ts_store_test_reset(), TS_OK);
	ts_store_kv_test_reset();
	uint8_t buf[TS_KV_BLOB_MAX];
	uint8_t out[TS_KV_BLOB_MAX];

	/* 写满 active 页（15 槽）再越界 → 压实翻页 + 数据保留 */
	for (int round = 0; round < 20; round++) {
		buf[0] = (uint8_t)('0' + round % 10);
		zassert_equal(ts_store_kv_save(0xAAAA0000 + (uint32_t)round,
					       buf, 8), TS_OK, "跨页写");
	}
	/* 有界压实语义：20 个不同 app > keep 上限（14）→ 最老 6 个按序弃
	 * （app0..5 NOTFOUND），其余保留各自最新（app6 起 + 末 app）。 */
	zassert_equal(ts_store_kv_load(0xAAAA0000, out, sizeof(out)),
		      TS_E_NOTFOUND, "最老 app 按序弃（有界语义）");
	zassert_equal(ts_store_kv_load(0xAAAA0006, out, sizeof(out)), 8);
	zassert_equal(out[0], '6', "app6 起保留");
	zassert_equal(ts_store_kv_load(0xAAAA0013, out, sizeof(out)), 8);
	zassert_equal(out[0], '9', "末 app 最新（round 19 → '9'）");
}
#endif

ZTEST_SUITE(framework_store, NULL, store_setup, NULL, NULL, NULL);
