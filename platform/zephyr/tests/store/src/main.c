/* SPDX-License-Identifier: Apache-2.0 */
/* R03 存储子闭环 1 测试（oracle 迁自旧 firmware/tests/store/src/main.c 的
 * meta/noinit 用例 + 新故障类别负测：越界/回绕/双损/CRC/one-shot/超长/
 * system_fail 留痕接线）。RAM 后端 = 测试隔离面，不冒充持久介质。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/kv.h>
#include <entelea/prov.h>
#include <entelea/trust.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"

#ifdef CONFIG_FLASH_MAP
#include "flash_store.h"
#endif

static void *store_setup(void)
{
#ifdef CONFIG_FLASH_MAP
	/* 板级批变体：先挂 flash 后端（RAM 模板尺寸冻结→布局映射），
	 * 再整区擦=净基线（test_reset 不卸载后端——擦除即隔离）。 */
	zassert_equal(entelea_platform_storage_mount_flash(), ENTELEA_OK,
		      "flash backend mount");
#endif
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK, "backend reset");
	return NULL;
}

/* ---- meta：往返（≥3 轮覆盖 copy0 较新路径）+ 撕裂 + 双损 ---- */
ZTEST(framework_store, test_meta_roundtrip_and_tear)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	uint8_t buf[64];
	uint16_t len = 0;

	/* 空：双副本皆无 → E_IO（不伪造缺省成功）。 */
	zassert_equal(entelea_storage_meta_read(buf, &len), ENTELEA_E_IO);

	/* 写读往返 ×6（seq 递增切换副本；MD1.1b 回归：第 3/5 轮后 copy0
	 * 较新，旧实现共用读缓冲读回旧副本内容）。 */
	for (int round = 0; round < 6; round++) {
		uint8_t w[32];

		for (int i = 0; i < 32; i++) {
			w[i] = (uint8_t)(round * 32 + i);
		}
		zassert_equal(entelea_storage_meta_write(w, 32), ENTELEA_OK);
		uint8_t r[64];

		zassert_equal(entelea_storage_meta_read(r, &len), ENTELEA_OK);
		zassert_equal(len, 32);
		zassert_ok(memcmp(r, w, 32), "round %d", round);
	}

	/* 撕裂注入：破坏活动副本 → 另一副本接管（6 轮末活动 = copy1）。 */
	zassert_equal(entelea_storage_meta_corrupt_test(1), ENTELEA_OK);
	uint8_t r[64];

	zassert_equal(entelea_storage_meta_read(r, &len), ENTELEA_OK,
		      "单副本损坏可恢复");
	zassert_equal(len, 32);

	/* 双副本皆损 → E_IO（启动按缺省安全态，不自动"修复"）。 */
	zassert_equal(entelea_storage_meta_corrupt_test(0), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(r, &len), ENTELEA_E_IO);

	/* 撕裂后写恢复：单副本存活 → 写非活动副本继续。 */
	uint8_t w2[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	zassert_equal(entelea_storage_meta_write(w2, 8), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(r, &len), ENTELEA_OK);
	zassert_equal(len, 8);
	zassert_ok(memcmp(r, w2, 8));
}

/* ---- meta：参数/边界负测 ---- */
ZTEST(framework_store, test_meta_params_and_bounds)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	uint8_t buf[ENTELEA_STORAGE_META_MAX + 1];
	uint16_t len = 0;

	zassert_equal(entelea_storage_meta_write(NULL, 4), ENTELEA_E_PARAM);
	zassert_equal(entelea_storage_meta_write(buf, 0), ENTELEA_E_PARAM);
	zassert_equal(entelea_storage_meta_write(buf, ENTELEA_STORAGE_META_MAX + 1),
		      ENTELEA_E_PARAM, "超容量拒绝");
	zassert_equal(entelea_storage_meta_write(buf, ENTELEA_STORAGE_META_MAX),
		      ENTELEA_OK, "恰满容量可写");
	zassert_equal(entelea_storage_meta_read(NULL, &len), ENTELEA_E_PARAM);
	zassert_equal(entelea_storage_meta_read(buf, NULL), ENTELEA_E_PARAM);
	/* 分区端口越界（u32 回绕含在内）：off/len 越界拒绝。 */
	uint8_t scratch[8];

	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_META,
							  0xFFFFFFF0U, scratch,
							  0x20),
		      ENTELEA_E_RANGE, "越界读拒绝（off+len 回绕）");
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_META, 0,
							  scratch, 0x100000),
		      ENTELEA_E_RANGE, "len 越界拒绝");
	zassert_equal(entelea_storage_test_backend_read(
			      (entelea_store_part_t)ENTELEA_PART_COUNT, 0,
			      scratch, 4),
		      ENTELEA_E_RANGE, "未启用分区拒绝（namespace 隔离）");
}

/* ---- noinit：fresh 语义 + one-shot + 超长 + 接线 ---- */
ZTEST(framework_store, test_noinit_semantics)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	uint16_t len = 0;
	bool fresh = true;
	uint8_t rec[8];

	zassert_equal(entelea_storage_noinit_get(rec, &len, &fresh), ENTELEA_OK);
	zassert_true(fresh, "空 = fresh");

	static const uint8_t crash[6] = {0x02, 0x00, 0x12, 0x34, 0x00, 0x01};

	entelea_storage_noinit_put(crash, 6);
	zassert_equal(entelea_storage_noinit_get(rec, &len, &fresh), ENTELEA_OK);
	zassert_false(fresh, "留痕存在");
	zassert_equal(len, 6);
	zassert_ok(memcmp(rec, crash, 6));

	/* one-shot：读后抹除，二次读 = fresh。 */
	fresh = false;
	zassert_equal(entelea_storage_noinit_get(rec, &len, &fresh), ENTELEA_OK);
	zassert_true(fresh, "one-shot：读后抹除");

	/* 超长留痕（>MAX）：拒绝写（丢弃），读取面保持无留痕。 */
	uint8_t big[ENTELEA_STORAGE_NOINIT_MAX_REC + 1];

	entelea_storage_noinit_put(big, sizeof(big));
	fresh = false;
	zassert_equal(entelea_storage_noinit_get(rec, &len, &fresh), ENTELEA_OK);
	zassert_true(fresh, "超长留痕不落盘");
	zassert_equal(entelea_storage_noinit_get(NULL, &len, &fresh),
		      ENTELEA_E_PARAM);
}

/* ---- R02 遗留接线：system_fail 原因 → noinit → init 消费 ---- */
ZTEST(framework_store, test_system_fail_reason_persisted)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_safety_test_reset();
	entelea_wdt_test_reset();
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(2));

	uint8_t rec[8];
	uint16_t len = 0;
	bool fresh = true;

	zassert_equal(entelea_storage_noinit_get(rec, &len, &fresh), ENTELEA_OK);
	zassert_false(fresh, "system_fail 必须留痕");
	zassert_equal(len, sizeof(uint32_t));
	uint32_t reason = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) |
			  ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);

	zassert_equal(reason, ENTELEA_FAIL_BOOT(2), "reason 编码往返");
	/* init 消费面：last_reset 呈现（init 前先复位留痕——上一 get 已消费，
	 * 重新落一次再走 init）。 */
	entelea_safety_system_fail(ENTELEA_FAIL_WDT(0));
	uint32_t got = 0;
	bool present = true;

	entelea_storage_last_reset(&got, &present);
	zassert_false(present, "init 未跑 = 不呈现");
	zassert_equal(entelea_storage_init(), ENTELEA_OK);
	entelea_storage_last_reset(&got, &present);
	zassert_true(present, "init 消费留痕");
	zassert_equal(got, ENTELEA_FAIL_WDT(0));
	entelea_wdt_test_reset();
}

static uint64_t audit_virt_now;

static entelea_result_t audit_virt_read(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	*out = audit_virt_now;
	return ENTELEA_OK;
}

static entelea_clock_t audit_clock = {
	.read_ms = audit_virt_read,
	.user = NULL,
	.clock_id = 0xA0D0,
	.last_ms = 0,
	.sampled = false,
};

/* ---- 审计两页乒乓环（子闭环 2） ---- */
ZTEST(framework_store, test_audit_ring_flip_and_recovery)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	zassert_not_equal(entelea_storage_audit_append(
				  &(entelea_storage_audit_entry_t){0}, 0),
			  ENTELEA_OK, "未 init 前 append 拒绝");
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);

	entelea_storage_audit_entry_t e = {.ch_idx = 7, .res = -4, .value_u = 42};
	entelea_storage_audit_entry_t out[130];

	for (uint32_t i = 1; i <= 65; i++) {
		e.t_ms = i;
		zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	}
	/* 翻页后两页过渡态：65 条全部可见（≤128）。 */
	size_t n = entelea_storage_audit_read(out, 130);

	zassert_equal(n, 65, "两页过渡态全部可见");
	zassert_equal(out[0].seq, 1);
	zassert_equal(out[64].seq, 65, "seq 升序");

	/* 恰满 128：两页全满瞬间全部可见（覆盖最旧的边界形态）。 */
	for (uint32_t i = 66; i <= 128; i++) {
		e.t_ms = i;
		zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	}
	n = entelea_storage_audit_read(out, 130);
	zassert_equal(n, 128, "两页全满=128");
	zassert_equal(out[0].seq, 1);
	/* 翻页第 129 条：页首写擦旧页（1..64 丢失）→ 可见=64+2。 */
	for (uint32_t i = 129; i <= 130; i++) {
		e.t_ms = i;
		zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	}
	n = entelea_storage_audit_read(out, 130);
	zassert_equal(n, 66, "翻页后旧页被擦（保留 ~64/瞬态 128 语义）");
	zassert_equal(out[0].seq, 65, "最旧段被翻页覆盖");
	zassert_equal(out[n - 1].seq, 130);

	/* 重启恢复（游标续接）：re-init 后 seq 继续。 */
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	n = entelea_storage_audit_read(out, 130);
	zassert_equal(out[n - 1].seq, 131, "reboot 后 seq 续接");

	/* 半写断尾：破坏活动页槽0（seq129）→ 该页断尾，仅另一页可见。
	 * 注入手法=先 erase_off 再写坏槽（NOR 纪律：裸覆写已编程区会被
	 * 擦除纪律拒——RAM 后端等价：64B 置 FF 后覆写）。 */
	uint8_t garbage[64] = {0xDE, 0xAD};

	zassert_equal(
		entelea_store_backend.erase_off(ENTELEA_PART_AUDIT, 0, 64),
		ENTELEA_OK,
		"注入预擦（flash=页0 扇区超集；RAM=64B 清——双后端等价）");
	zassert_equal(
		entelea_storage_test_backend_write(ENTELEA_PART_AUDIT, 0, garbage,
						   64),
		ENTELEA_OK, "撕裂注入：破坏槽0");
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	n = entelea_storage_audit_read(out, 130);
	zassert_equal(n, 64, "断尾后仅存另一页连续段");
	zassert_equal(out[n - 1].seq, 128);
}

ZTEST(framework_store, test_safety_exception_audit_wiring)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_safety_test_reset();
	entelea_wdt_test_reset();
	audit_virt_now = 1000;
	zassert_equal(entelea_safety_bind_clock(&audit_clock), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);

	static const entelea_out_channel_t ch = {
		.uid = "aud", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = true},
	};
	zassert_equal(entelea_safety_register_channel(&ch), ENTELEA_OK);

	/* commit 异常（安全态拒绝）→ 审计 evt=0。 */
	zassert_equal(entelea_safety_commit("aud", (entelea_out_value_t){.b = true}),
		      ENTELEA_E_STATE);
	entelea_storage_audit_entry_t out[4];
	size_t n = entelea_storage_audit_read(out, 4);

	zassert_equal(n, 1, "首条异常立即落盘");
	zassert_equal(out[0].evt, 0);
	zassert_equal(out[0].res, ENTELEA_E_STATE);
	zassert_equal(out[0].ch_idx, 0);

	/* 同窗口再拒绝 → 节流丢弃（计数可观），环不增长。 */
	zassert_equal(entelea_safety_commit("aud", (entelea_out_value_t){.b = true}),
		      ENTELEA_E_STATE);
	zassert_equal(entelea_safety_audit_throttled(), 1, "风暴节流计数");
	n = entelea_storage_audit_read(out, 4);
	zassert_equal(n, 1, "节流期不落盘");

	/* estop 沿 → evt=1（不节流）。补发由巡检驱动——测试直调补发面。 */
	entelea_safety_force_all_fault();
	entelea_safety_estop_deferred_publish();
	n = entelea_storage_audit_read(out, 4);
	zassert_equal(n, 2, "estop 沿入环");
	zassert_equal(out[1].evt, 1);
	zassert_equal(out[1].ch_idx, 0xFFFF, "系统事件");
	entelea_wdt_test_reset();
}

/* ---- prov：CBOR 构造 + oracle（迁自旧 T-STORE prov 用例族） ---- */
#define CB_MAX 2048
struct cb {
	uint8_t b[CB_MAX];
	uint32_t n;
};

static void cb_u8(struct cb *c, uint8_t v) { c->b[c->n++] = v; }

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

static void build_prov(struct cb *c, const char *node, const char *cube,
		       const char *router, uint32_t pwr, int ver,
		       uint8_t deploy_mode, uint8_t remote_clear, int lconf,
		       const uint32_t *tls)
{
	memset(c, 0, sizeof(*c));
	uint32_t pairs = 9;

	if (ver == 2) {
		pairs = (tls != NULL) ? 15 : ((lconf >= 0) ? 12 : 11);
	}
	cb_hdr(c, 0xA0, pairs);
	cb_tstr(c, "v");
	cb_hdr(c, 0x00, (uint32_t)ver);
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
		cb_tstr(c, "deploy_mode");
		cb_hdr(c, 0x00, deploy_mode);
		cb_tstr(c, "estop_remote_clear");
		cb_hdr(c, 0x00, remote_clear);
		if (lconf >= 0 || tls != NULL) {
			cb_tstr(c, "lconf");
			cb_hdr(c, 0x00, (uint32_t)(lconf >= 0 ? lconf : 0));
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

ZTEST(framework_store, test_prov_v1_and_defaults)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_prov_maint_test_reset();
	struct cb c;

	build_prov(&c, "node-1", "cube-9", "tcp/10.0.0.1:7447", 500, 1, 0, 0, -1,
		   NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_OK);
	const entelea_prov_t *p = entelea_prov();

	zassert_ok(strcmp(p->node_id, "node-1"));
	zassert_ok(strcmp(p->cube_id, "cube-9"));
	zassert_ok(strcmp(p->router_locators[0], "tcp/10.0.0.1:7447"));
	zassert_equal(p->router_locators[1][0], '\0', "routers 余位清零");
	zassert_equal(p->power_budget_ma, 500);
	zassert_equal(p->estop_trigger_flags, 1);
	zassert_equal(p->root_pubkeys[0][0], 0x10);
	zassert_equal(p->root_pubkeys[1][0], 0x60);
	zassert_ok(strcmp(p->zenoh_cred, "test-cred"));
	/* v1 缺字段缺省（现网兼容）。 */
	zassert_equal(p->deploy_mode, ENTELEA_DEPLOY_ORCHESTRATED);
	zassert_equal(p->estop_remote_clear, 1);
	zassert_equal(p->local_confirm, 0);
	zassert_equal(p->tls_ca_len, 0, "v1 = TLS off");
}

ZTEST(framework_store, test_prov_v2_domain_and_schema)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_prov_maint_test_reset();
	struct cb c;

	build_prov(&c, "n-v2", "c-v2", "tcp/10.0.0.2:7447", 200, 2, 1, 0, -1,
		   NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_OK);
	zassert_equal(entelea_prov()->deploy_mode, ENTELEA_DEPLOY_STANDALONE);
	zassert_equal(entelea_prov()->estop_remote_clear, 0);

	/* 域外值 fail-closed（未知模式/清除/lconf 不静默归并）。 */
	build_prov(&c, "n-x", "c-x", "tcp/x:1", 1, 2, 2, 1, -1, NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "deploy_mode=2 拒");
	build_prov(&c, "n-y", "c-y", "tcp/x:1", 1, 2, 1, 2, -1, NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "remote_clear=2 拒");
	build_prov(&c, "n-l", "c-l", "tcp/x:1", 1, 2, 0, 1, 2, NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "lconf=2 拒");

	/* v/pairs 错配：v1 9 对 + v=2。 */
	build_prov(&c, "n-z", "c-z", "tcp/x:1", 1, 1, 0, 0, -1, NULL);
	c.b[3] = 0x02;
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "v/pairs 错配拒");

	/* 尾部余量字节拒（超集）。 */
	build_prov(&c, "n-t", "c-t", "tcp/x:1", 1, 1, 0, 0, -1, NULL);
	c.b[c.n++] = 0x00;
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "尾部余量拒");

	/* CRC 失败：破坏 CBOR 一字节（头仍自洽）→ E_IO。 */
	build_prov(&c, "n-c", "c-c", "tcp/x:1", 1, 1, 0, 0, -1, NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	/* CRC 破坏注入（NOR 合法）：擦区后写"头自洽+错 CRC+真 CBOR"
	 * 记录——就地翻位在擦除纪律下不可编程（一擦一写）；RAM 等价。 */
	{
		static uint8_t rec[4096];
		uint32_t n = c.n;

		zassert_equal(entelea_storage_erase(ENTELEA_PART_PROV),
			      ENTELEA_OK);
		rec[0] = (uint8_t)n;
		rec[1] = (uint8_t)(n >> 8);
		rec[2] = (uint8_t)(n >> 16);
		rec[3] = (uint8_t)(n >> 24);
		rec[4] = 0xBEEF & 0xFF; /* 任意非匹配 CRC */
		rec[5] = 0xDE;
		memcpy(rec + 6, c.b, n);
		zassert_equal(
			entelea_storage_write(ENTELEA_PART_PROV, 0, rec, 6 + n),
			ENTELEA_OK);
	}
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "CRC 失败拒");
}

ZTEST(framework_store, test_prov_lconf_tls_and_maint)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_prov_maint_test_reset();
	struct cb c;

	/* lconf 12 对 + TLS 15 对形态与长度镜像。 */
	build_prov(&c, "n-lc", "c-lc", "tcp/x:1", 1, 2, 0, 1, 1, NULL);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_OK);
	zassert_equal(entelea_prov()->local_confirm, 1);
	const uint32_t tls[3] = {600, 620, 121};

	build_prov(&c, "n-tls", "c-tls", "tcp/x:1", 1, 2, 0, 1, 0, tls);
	zassert_equal(entelea_prov_write_test(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_OK);
	zassert_equal(entelea_prov()->tls_ca_len, 600);
	zassert_equal(entelea_prov()->tls_cert_len, 620);
	zassert_equal(entelea_prov()->tls_key_len, 121);
	zassert_equal(entelea_prov_tls()->ca_len, 600, "装载态长度镜像");
	zassert_equal(entelea_prov_tls()->key[0], 0xC0);

	/* 维护窗：非法 blob 零写入（分区保持空 → load E_IO）。 */
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_prov_maint_test_reset();
	build_prov(&c, "n", "c", "r", 1, 1, 0, 0, -1, NULL);
	c.b[3] ^= 0xFF;
	zassert_equal(entelea_prov_write_maint(c.b, c.n), ENTELEA_E_IO);
	zassert_equal(entelea_prov_load(), ENTELEA_E_IO, "零写入（分区仍空）");

	/* 合法 → 写入装载；一次性窗；复位模拟后再写。 */
	build_prov(&c, "mn-7", "mc-7", "tcp/10.1.1.1:9955", 700, 1, 0, 0, -1,
		   NULL);
	zassert_equal(entelea_prov_write_maint(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_OK);
	zassert_ok(strcmp(entelea_prov()->node_id, "mn-7"));
	build_prov(&c, "mn-8", "mc-8", "tcp/10.1.1.2:9955", 800, 1, 0, 0, -1,
		   NULL);
	zassert_equal(entelea_prov_write_maint(c.b, c.n), ENTELEA_E_STATE,
		      "per-boot 一次性窗");
	entelea_prov_maint_test_reset();
	zassert_equal(entelea_prov_write_maint(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_prov_load(), ENTELEA_OK);
	zassert_ok(strcmp(entelea_prov()->node_id, "mn-8"));
}

/* ---- 信任表（子闭环 4）：schema 矩阵/乒乓/撕裂 ---- */
static void build_trust(struct cb *c, uint32_t ver, int nkeys, int dup_at)
{
	memset(c, 0, sizeof(*c));
	cb_hdr(c, 0xA0, 2);
	cb_tstr(c, "ver");
	cb_hdr(c, 0x00, ver);
	cb_tstr(c, "keys");
	cb_hdr(c, 0x80, (uint32_t)nkeys);
	for (int i = 0; i < nkeys; i++) {
		cb_bstr32(c, (dup_at == i) ? 0x10 : (uint8_t)(0x10 + 4 * i));
	}
}

ZTEST(framework_store, test_trust_table)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trust_test_reset();
	struct cb c;
	entelea_trust_table_t t;

	zassert_equal(entelea_trust()->ver, 0, "初始空分区=工厂根哨兵");
	zassert_equal(entelea_trust()->n_keys, 0);

	build_trust(&c, 1, 2, -1);
	zassert_equal(entelea_trust_validate(c.b, c.n, &t), ENTELEA_OK);
	zassert_equal(t.ver, 1);
	zassert_equal(t.n_keys, 2);
	build_trust(&c, 0, 1, -1);
	zassert_equal(entelea_trust_validate(c.b, c.n, &t), ENTELEA_E_IO, "ver=0 拒");
	build_trust(&c, 1, 9, -1);
	zassert_equal(entelea_trust_validate(c.b, c.n, &t), ENTELEA_E_IO, "超 8 键拒");
	build_trust(&c, 1, 2, 1);
	zassert_equal(entelea_trust_validate(c.b, c.n, &t), ENTELEA_E_IO, "键内重复拒");
	build_trust(&c, 1, 2, -1);
	c.b[c.n++] = 0x00;
	zassert_equal(entelea_trust_validate(c.b, c.n, &t), ENTELEA_E_IO, "尾部余量拒");

	build_trust(&c, 1, 2, -1);
	zassert_equal(entelea_trust_write(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_trust()->ver, 1);
	zassert_equal(entelea_trust()->keys[0][0], 0x10);
	zassert_equal(entelea_trust()->keys[1][0], 0x14);

	build_trust(&c, 1, 9, -1);
	zassert_equal(entelea_trust_write(c.b, c.n), ENTELEA_E_IO, "坏表零写入");
	zassert_equal(entelea_trust()->n_keys, 2);

	build_trust(&c, 2, 1, -1);
	zassert_equal(entelea_trust_write(c.b, c.n), ENTELEA_OK);
	zassert_equal(entelea_trust()->ver, 2);

	entelea_trust_test_reset();
	zassert_equal(entelea_trust_corrupt_test(1), ENTELEA_OK);
	zassert_equal(entelea_trust_load(), ENTELEA_OK);
	zassert_equal(entelea_trust()->ver, 1, "撕裂回退旧副本");
	zassert_equal(entelea_trust()->n_keys, 2);

	zassert_equal(entelea_trust_corrupt_test(0), ENTELEA_OK);
	entelea_trust_test_reset();
	zassert_equal(entelea_trust_load(), ENTELEA_OK);
	zassert_equal(entelea_trust()->ver, 0, "双损=工厂根哨兵");
}

/* ---- kv：往返/隔离/压实/重装载 ---- */
ZTEST(framework_store, test_kv_roundtrip_compact_reload)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	uint8_t buf[ENTELEA_KV_BLOB_MAX];
	uint8_t w[64];

	/* 未存 = NOTFOUND；参数负测。 */
	zassert_equal(entelea_kv_load(0xAAAA, buf, sizeof(buf)),
		      (int32_t)ENTELEA_E_NOTFOUND);
	zassert_equal(entelea_kv_save(1, w, 0), ENTELEA_E_PARAM);
	zassert_equal(entelea_kv_save(1, w, ENTELEA_KV_BLOB_MAX + 1),
		      ENTELEA_E_PARAM);

	for (int app = 0; app < 3; app++) {
		for (int i = 0; i < 64; i++) {
			w[i] = (uint8_t)(app * 64 + i);
		}
		zassert_equal(entelea_kv_save(0x1000 + (uint32_t)app, w, 64),
			      ENTELEA_OK);
	}
	for (int app = 0; app < 3; app++) {
		int32_t n = entelea_kv_load(0x1000 + (uint32_t)app, buf, sizeof(buf));

		zassert_equal(n, 64, "app %d 往返", app);
		zassert_equal(buf[0], (uint8_t)(app * 64), "隔离（app %d）", app);
	}

	/* 覆盖写（同 app 取最新）。 */
	for (int i = 0; i < 64; i++) {
		w[i] = 0xEE;
	}
	zassert_equal(entelea_kv_save(0x1000, w, 8), ENTELEA_OK);
	zassert_equal(entelea_kv_load(0x1000, buf, sizeof(buf)), 8);
	zassert_equal(buf[0], 0xEE);
	zassert_equal(entelea_kv_load(0x1000, buf, 4), (int32_t)ENTELEA_E_RANGE,
		      "容量不足如实");

	/* 压实翻页：把页 0 写满（15 槽）→ 触发压实 → 重装载后数据仍在。 */
	for (int round = 0; round < 14; round++) {
		w[0] = (uint8_t)round;
		zassert_equal(entelea_kv_save(0x2000, w, 16), ENTELEA_OK);
		zassert_equal(entelea_kv_save(0x3000, w, 16), ENTELEA_OK);
	}
	entelea_kv_test_reset(); /* 模拟复位：重扫描 */
	zassert_equal(entelea_kv_init(), ENTELEA_OK);
	zassert_equal(entelea_kv_load(0x1000, buf, sizeof(buf)), 8);
	zassert_equal(buf[0], 0xEE, "重装载后最新保留");
	zassert_true(entelea_kv_load(0x2000, buf, sizeof(buf)) == 16,
		     "压实后 app 存活");
}

/* ---- 启动表：storage_init 尾部追加 ---- */
ZTEST(framework_store, test_boot_table_has_storage_step)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	zassert_equal(ENTELEA_PLATFORM_BOOT_STEP_COUNT, 6);
	zassert_ok(strcmp(entelea_platform_boot_steps[5].name, "storage_init"));
	zassert_not_null(entelea_platform_boot_steps[5].fn);
}

/* ---- META 跨逻辑副本边界（板级批 review P2-1）：分段翻译正确 ---- */
ZTEST(framework_store, test_meta_boundary_cross_rw)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	uint8_t w[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t r[8] = {0};

	zassert_equal(
		entelea_store_backend.write(ENTELEA_PART_META, 508, w, 8),
		ENTELEA_OK, "跨界写（copy0 尾 4B+copy1 头 4B）");
	zassert_equal(entelea_store_backend.read(ENTELEA_PART_META, 508, r, 8),
		      ENTELEA_OK);
	zassert_ok(memcmp(r, w, 8),
		   "跨界读回=逻辑连续语义（flash 分段翻译不落垫片）");
	/* copy1 头部独立读（物理 4096 处，逻辑 512）。 */
	zassert_equal(entelea_store_backend.read(ENTELEA_PART_META, 512, r, 4),
		      ENTELEA_OK);
	zassert_equal(r[0], 5, "copy1 头=写序第二段");
}

ZTEST_SUITE(framework_store, NULL, store_setup, NULL, NULL, NULL);
