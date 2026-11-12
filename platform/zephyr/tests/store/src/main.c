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
	zassert_equal(entelea_storage_meta_read(buf, sizeof(buf), &len), ENTELEA_E_IO);

	/* 写读往返 ×6（seq 递增切换副本；MD1.1b 回归：第 3/5 轮后 copy0
	 * 较新，旧实现共用读缓冲读回旧副本内容）。 */
	for (int round = 0; round < 6; round++) {
		uint8_t w[32];

		for (int i = 0; i < 32; i++) {
			w[i] = (uint8_t)(round * 32 + i);
		}
		zassert_equal(entelea_storage_meta_write(w, 32), ENTELEA_OK);
		uint8_t r[64];

		zassert_equal(entelea_storage_meta_read(r, sizeof(r), &len), ENTELEA_OK);
		zassert_equal(len, 32);
		zassert_ok(memcmp(r, w, 32), "round %d", round);
	}

	/* 撕裂注入：破坏活动副本 → 另一副本接管（6 轮末活动 = copy1）。 */
	zassert_equal(entelea_storage_meta_corrupt_test(1), ENTELEA_OK);
	uint8_t r[64];

	zassert_equal(entelea_storage_meta_read(r, sizeof(r), &len), ENTELEA_OK,
		      "单副本损坏可恢复");
	zassert_equal(len, 32);

	/* 双副本皆损 → E_IO（启动按缺省安全态，不自动"修复"）。 */
	zassert_equal(entelea_storage_meta_corrupt_test(0), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(r, sizeof(r), &len), ENTELEA_E_IO);

	/* 撕裂后写恢复：单副本存活 → 写非活动副本继续。 */
	uint8_t w2[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	zassert_equal(entelea_storage_meta_write(w2, 8), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(r, sizeof(r), &len), ENTELEA_OK);
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
	zassert_equal(entelea_storage_meta_read(NULL, sizeof(buf), &len), ENTELEA_E_PARAM);
	zassert_equal(entelea_storage_meta_read(buf, sizeof(buf), NULL), ENTELEA_E_PARAM);
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

ZTEST(framework_store, test_mirrored_meta_requires_matching_complete_copies)
{
	const uint8_t original[] = {1, 2, 3, 4};
	const uint8_t changed[] = {4, 3, 2, 1};
	uint8_t out[sizeof(original)] = {0xAA, 0xAA, 0xAA, 0xAA};
	uint16_t len = 0xBEEF;

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read_mirrored(NULL, sizeof(out), &len),
		      ENTELEA_E_PARAM);
	zassert_equal(entelea_storage_meta_read_mirrored(out, sizeof(out), NULL),
		      ENTELEA_E_PARAM);
	zassert_equal(entelea_storage_meta_write(original, sizeof(original)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read_mirrored(out, sizeof(out), &len),
		      ENTELEA_E_IO, "one valid copy is not mirrored");
	zassert_equal(len, 0xBEEF);
	zassert_equal(entelea_storage_meta_write(original, sizeof(original)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read_mirrored(out, sizeof(out) - 1, &len),
		      ENTELEA_E_RANGE);
	zassert_equal(out[0], 0xAA, "capacity failure must not write output");
	zassert_equal(len, 0xBEEF);
	zassert_equal(entelea_storage_meta_read_mirrored(out, sizeof(out), &len),
		      ENTELEA_OK);
	zassert_equal(len, sizeof(original));
	zassert_mem_equal(out, original, sizeof(original));
	zassert_equal(entelea_storage_meta_write(changed, sizeof(changed)), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read_mirrored(out, sizeof(out), &len),
		      ENTELEA_E_IO, "different valid copies are ambiguous");
	zassert_mem_equal(out, original, sizeof(original));
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
	int32_t n = entelea_storage_audit_read(out, 130);

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

	/* 坏槽不断尾（RF1 合同变更，review P1-6）：旧实现自首个坏槽起整页
	 * 截断——烧毁槽之后的有效记录被丢弃；旧注入（64B erase_off）在
	 * flash 上擦整页、在 RAM 上只清 64B，双后端语义不同。现注入双后端
	 * 等价：整页 0 擦除 → 槽0 垃圾 + 槽1 手工编码有效记录（seq 200）。 */
	uint8_t garbage[64];
	uint8_t rec[64];

	memset(garbage, 0xFF, sizeof(garbage));
	garbage[0] = 0xDE;
	garbage[1] = 0xAD;
	memset(rec, 0xFF, sizeof(rec));
	rec[0] = 0x31; /* magic "TSA1" LE */
	rec[1] = 0x41;
	rec[2] = 0x53;
	rec[3] = 0x54;
	rec[4] = 200; /* seq LE */
	rec[5] = rec[6] = rec[7] = 0;
	rec[16] = 7; /* ch_idx LE */
	rec[17] = 0;
	zassert_equal(entelea_store_backend.erase_off(ENTELEA_PART_AUDIT, 0, 4096),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_AUDIT, 0,
							 garbage, 64),
		      ENTELEA_OK, "坏槽注入");
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_AUDIT, 64,
							 rec, 64),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	n = entelea_storage_audit_read(out, 130);
	zassert_equal(n, 65, "坏槽不可见、其后有效槽可见（64+1）");
	zassert_equal(out[0].seq, 65);
	zassert_equal(out[n - 1].seq, 200);
	zassert_equal(out[n - 1].ch_idx, 7);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	n = entelea_storage_audit_read(out, 130);
	zassert_equal(n, 66);
	zassert_equal(out[n - 1].seq, 201, "续接=有效最大+1，坏槽不复用");
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
	int32_t n = entelea_storage_audit_read(out, 4);

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
	zassert_ok(strcmp(entelea_platform_boot_steps[4].name, "storage_init"));
	zassert_not_null(entelea_platform_boot_steps[4].fn);
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

/* ---- RF1 G2：KV/审计掉电撕裂与介质读错误（review P1-5/P1-6 回归） ----
 * 注入后端包裹当前后端（RAM/flash 双变体同一手法）：第 k 次写调用只写
 * 前缀后报 E_IO（撕裂）；读错误开关；NOR 纪律——覆写已编程字节即计数
 * 并拒绝（RAM 变体同样检出二次编程，flash 变体与模拟器纪律一致）。 */
static entelea_store_ops_t inj_base;
static bool inj_mounted;
static entelea_store_ops_t bit_error_base;
static bool bit_error_mounted;
static entelea_store_part_t bit_error_part;
static uint32_t bit_error_off;
static uint8_t bit_error_mask;
static struct {
	uint32_t writes;
	uint32_t tear_at;
	uint32_t tear_keep;
	uint32_t dbl;
	bool read_fail;
} inj;

static entelea_result_t inj_read(entelea_store_part_t part, uint32_t off,
				 void *buf, uint32_t len)
{
	if (inj.read_fail) {
		return ENTELEA_E_IO;
	}
	return inj_base.read(part, off, buf, len);
}

static entelea_result_t inj_write(entelea_store_part_t part, uint32_t off,
				  const void *buf, uint32_t len)
{
	uint8_t cur[64];

	for (uint32_t done = 0; done < len; done += sizeof(cur)) {
		uint32_t n = (len - done < sizeof(cur)) ? len - done : sizeof(cur);

		if (inj_base.read(part, off + done, cur, n) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (cur[i] != 0xFF) {
				inj.dbl++;
				return ENTELEA_E_IO;
			}
		}
	}
	inj.writes++;
	if (inj.tear_at != 0 && inj.writes == inj.tear_at) {
		inj.tear_at = 0;
		if (inj.tear_keep > 0) {
			(void)inj_base.write(part, off, buf,
					     inj.tear_keep < len ? inj.tear_keep : len);
		}
		return ENTELEA_E_IO;
	}
	return inj_base.write(part, off, buf, len);
}

static entelea_result_t inj_erase(entelea_store_part_t part)
{
	return inj_base.erase(part);
}

static entelea_result_t inj_erase_off(entelea_store_part_t part, uint32_t off,
				      uint32_t len)
{
	return inj_base.erase_off(part, off, len);
}

static uint32_t inj_size(entelea_store_part_t part)
{
	return inj_base.size(part);
}

static void inj_begin(void)
{
	static const entelea_store_ops_t ops = {
		.read = inj_read, .write = inj_write, .erase = inj_erase,
		.erase_off = inj_erase_off, .size = inj_size,
	};

	zassert_false(inj_mounted);
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	memset(&inj, 0, sizeof(inj));
	inj_base = entelea_store_backend;
	zassert_equal(entelea_storage_mount(&ops), ENTELEA_OK);
	inj_mounted = true;
}

/* 自本次起第 k 次写调用撕裂，只落 keep 字节。 */
static void inj_tear(uint32_t k, uint32_t keep)
{
	inj.writes = 0;
	inj.tear_at = k;
	inj.tear_keep = keep;
}

static void inj_end(void)
{
	zassert_equal(inj.dbl, 0, "零二次编程（NOR 擦后写纪律）");
	zassert_equal(entelea_storage_mount(&inj_base), ENTELEA_OK);
	inj_mounted = false;
}

static void store_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (inj_mounted) { /* 断言中止的用例：恢复后端，不殃及后续用例 */
		(void)entelea_storage_mount(&inj_base);
		inj_mounted = false;
	}
	if (bit_error_mounted) {
		(void)entelea_storage_mount(&bit_error_base);
		bit_error_mounted = false;
	}
}

#define APP1 0xA001U
#define APP2 0xA002U

static void kv_expect(uint32_t app, const void *val, int32_t len)
{
	uint8_t b[8];

	zassert_equal(entelea_kv_load(app, b, sizeof(b)), len, "app %x", app);
	zassert_mem_equal(b, val, (size_t)len, "app %x", app);
}

static entelea_result_t bit_error_read(entelea_store_part_t part, uint32_t off,
				       void *buf, uint32_t len)
{
	entelea_result_t r = bit_error_base.read(part, off, buf, len);

	if (r == ENTELEA_OK && part == bit_error_part && bit_error_off >= off &&
	    bit_error_off - off < len) {
		((uint8_t *)buf)[bit_error_off - off] ^= bit_error_mask;
	}
	return r;
}

static void flip_storage_bit(entelea_store_part_t part, uint32_t off, uint8_t mask)
{
	entelea_store_ops_t ops;

	zassert_false(bit_error_mounted);
	bit_error_base = entelea_store_backend;
	ops = bit_error_base;
	ops.read = bit_error_read;
	bit_error_part = part;
	bit_error_off = off;
	bit_error_mask = mask;
	zassert_equal(entelea_storage_mount(&ops), ENTELEA_OK);
	bit_error_mounted = true;
}

static void flip_storage_byte(entelea_store_part_t part, uint32_t off)
{
	flip_storage_bit(part, off, 1U);
}

static void clear_storage_flip(void)
{
	zassert_true(bit_error_mounted);
	zassert_equal(entelea_storage_mount(&bit_error_base), ENTELEA_OK);
	bit_error_mounted = false;
}

ZTEST(framework_store, test_committed_kv_bit_corruption_is_not_success)
{
	const uint32_t offsets[] = {0, 4, 8, 12, 14, 14 + 249,
				    3960, 3962, 3964};
	uint8_t value[ENTELEA_KV_BLOB_MAX];
	uint8_t out[ENTELEA_KV_BLOB_MAX];

	memset(value, 0x5A, sizeof(value));
	for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
		zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
		entelea_kv_test_reset();
		zassert_equal(entelea_kv_save(APP1, value, sizeof(value)), ENTELEA_OK);
		flip_storage_byte(ENTELEA_PART_KV, offsets[i]);
		memset(out, 0xA5, sizeof(out));
		zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), ENTELEA_E_IO,
			      "corrupted committed offset %u must not return data",
			      (unsigned)offsets[i]);
		zassert_equal(out[0], 0xA5);
		entelea_kv_test_reset();
		zassert_equal(entelea_kv_init(), ENTELEA_E_IO);
		zassert_equal(entelea_kv_save(APP2, value, sizeof(value)), ENTELEA_E_IO);
		clear_storage_flip();
	}
}

ZTEST(framework_store, test_committed_audit_bit_corruption_is_not_success)
{
	const uint32_t offsets[] = {0, 4, 8, 16, 20, 22, 24, 28, 32, 34, 36};
	entelea_storage_audit_entry_t entry = {
		.t_ms = 25, .ch_idx = 3, .actor = 4, .kind = 2,
		.res = -4, .value_u = 9,
	};
	entelea_storage_audit_entry_t out[1];

	for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
		zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
		zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
		zassert_equal(entelea_storage_audit_append(&entry, 7), ENTELEA_OK);
		flip_storage_byte(ENTELEA_PART_AUDIT, offsets[i]);
		memset(out, 0xA5, sizeof(out));
		zassert_equal(entelea_storage_audit_read(out, 1), ENTELEA_E_IO,
			      "corrupted committed offset %u must not be reported",
			      (unsigned)offsets[i]);
		zassert_equal(((uint8_t *)out)[0], 0xA5);
		zassert_equal(entelea_storage_audit_init(), ENTELEA_E_IO);
		zassert_equal(entelea_storage_audit_append(&entry, 7), ENTELEA_E_STATE);
		clear_storage_flip();
	}
}

ZTEST(framework_store, test_every_protected_bit_detected)
{
	uint8_t value[ENTELEA_KV_BLOB_MAX];
	uint8_t out[ENTELEA_KV_BLOB_MAX];
	entelea_storage_audit_entry_t entry = {.value_u = 42};
	entelea_storage_audit_entry_t audit[1];

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	memset(value, 0x5A, sizeof(value));
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"OLD", 3), ENTELEA_OK);
	zassert_equal(entelea_kv_save(APP1, value, sizeof(value)), ENTELEA_OK);
	for (uint32_t pos = 0; pos < 264 + 8; pos++) {
		uint32_t off = pos < 264 ? 264 + pos : 3960 + 8 + pos - 264;

		for (uint8_t bit = 0; bit < 8; bit++) {
			zassert_equal(entelea_kv_init(), ENTELEA_OK);
			flip_storage_bit(ENTELEA_PART_KV, off, (uint8_t)(1U << bit));
			zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), ENTELEA_E_IO,
				      "KV byte %u bit %u", (unsigned)pos, bit);
			clear_storage_flip();
		}
	}
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), sizeof(value));
	zassert_mem_equal(out, value, sizeof(value));

	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_append(&entry, 0), ENTELEA_OK);
	for (uint32_t pos = 0; pos < 40; pos++) {
		for (uint8_t bit = 0; bit < 8; bit++) {
			zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
			flip_storage_bit(ENTELEA_PART_AUDIT, pos, (uint8_t)(1U << bit));
			zassert_equal(entelea_storage_audit_read(audit, 1), ENTELEA_E_IO,
				      "audit byte %u bit %u", (unsigned)pos, bit);
			clear_storage_flip();
		}
	}
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_read(audit, 1), 1);
	zassert_equal(audit[0].value_u, 42);
}

ZTEST(framework_store, test_write_readback_mismatch_is_not_success)
{
	uint8_t out[8];
	entelea_storage_audit_entry_t entry = {.value_u = 42};
	entelea_storage_audit_entry_t audit[1];

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_init(), ENTELEA_OK);
	flip_storage_byte(ENTELEA_PART_KV, 14);
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"DATA", 4), ENTELEA_E_IO);
	clear_storage_flip();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), 4);
	zassert_mem_equal(out, "DATA", 4, "failed readback leaves effect unknown");

	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	flip_storage_byte(ENTELEA_PART_AUDIT, 28);
	zassert_equal(entelea_storage_audit_append(&entry, 0), ENTELEA_E_IO);
	clear_storage_flip();
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_read(audit, 1), 1);
	zassert_equal(audit[0].value_u, 42);
}

ZTEST(framework_store, test_legacy_kv_survives_compaction_without_relabelling)
{
	uint8_t legacy[264];
	uint8_t tag[8];
	uint8_t old_format[264];
	uint8_t out[8];
	uint8_t v;

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	memset(legacy, 0xFF, sizeof(legacy));
	legacy[0] = 0x31; legacy[1] = 0x4B; legacy[2] = 0x53; legacy[3] = 0x54;
	legacy[4] = 0x01; legacy[5] = 0xA0; legacy[6] = 0; legacy[7] = 0;
	legacy[8] = 1; legacy[9] = legacy[10] = legacy[11] = 0;
	legacy[12] = 3; legacy[13] = 0;
	memcpy(legacy + 14, "OLD", 3);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 0,
							legacy, sizeof(legacy)),
		      ENTELEA_OK);
	kv_expect(APP1, "OLD", 3);
	for (v = 0; v < 15; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK);
	}
	entelea_kv_test_reset();
	kv_expect(APP1, "OLD", 3);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 4096 + 3960,
						       tag, sizeof(tag)),
		      ENTELEA_OK);
	for (size_t i = 0; i < sizeof(tag); i++) {
		zassert_equal(tag[i], 0xFF, "compaction must not claim legacy CRC");
	}
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW", 3), ENTELEA_OK);
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), 3);
	zassert_mem_equal(out, "NEW", 3);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV,
						       4096 + 3 * 264,
						       old_format, sizeof(old_format)),
		      ENTELEA_OK);
	zassert_mem_equal(old_format, legacy, 8, "legacy magic and app hash unchanged");
	zassert_equal(old_format[12], 3);
	zassert_mem_equal(old_format + 14, "NEW", 3);
	flip_storage_byte(ENTELEA_PART_KV, 4096 + 3 * 264 + 14);
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), ENTELEA_E_IO);
	clear_storage_flip();
}

#ifndef ENTELEA_TEST_FLASH_BACKEND
ZTEST(framework_store, test_ram_persisted_bit_corruption_across_reinit)
{
	uint8_t byte;
	uint8_t out[8];

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"DATA", 4), ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 14,
						       &byte, 1), ENTELEA_OK);
	byte ^= 1U;
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 14,
							&byte, 1), ENTELEA_OK);
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), ENTELEA_E_IO);
}
#endif

ZTEST(framework_store, test_legacy_audit_record_remains_readable)
{
	uint8_t legacy[64];
	uint8_t raw[64];
	entelea_storage_audit_entry_t out[2];
	entelea_storage_audit_entry_t newer = {.t_ms = 9, .ch_idx = 7, .value_u = 42};

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	memset(legacy, 0xFF, sizeof(legacy));
	legacy[0] = 0x31; legacy[1] = 0x41; legacy[2] = 0x53; legacy[3] = 0x54;
	legacy[4] = 1; legacy[5] = legacy[6] = legacy[7] = 0;
	legacy[8] = 5; memset(legacy + 9, 0, 7);
	legacy[16] = 3; legacy[17] = 0;
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_AUDIT, 0,
							legacy, sizeof(legacy)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_read(out, 2), 1);
	zassert_equal(out[0].seq, 1);
	zassert_equal(out[0].t_ms, 5);
	zassert_equal(out[0].ch_idx, 3);
	zassert_equal(entelea_storage_audit_append(&newer, 0), ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_AUDIT, 64,
						       raw, sizeof(raw)),
		      ENTELEA_OK);
	zassert_mem_equal(raw, legacy, 4, "legacy magic unchanged");
	zassert_equal(raw[4], 2);
	zassert_equal(raw[8], 9);
	zassert_equal(raw[16], 7);
	zassert_equal(raw[28], 42);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_read(out, 2), 2);
	zassert_equal(out[1].value_u, 42);
}

ZTEST(framework_store, test_failed_write_may_have_committed_full_effect)
{
	uint8_t out[8];
	entelea_storage_audit_entry_t e = {.value_u = 42};
	entelea_storage_audit_entry_t audit[1];

	inj_begin();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"OLD", 3), ENTELEA_OK);
	inj_tear(4, 4);
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW", 3), ENTELEA_E_IO);
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), 3);
	zassert_mem_equal(out, "NEW", 3, "reported error does not imply no effect");

	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	inj_tear(3, 4);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_E_IO);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_read(audit, 1), 1);
	zassert_equal(audit[0].value_u, 42);
	inj_end();
}

ZTEST(framework_store, test_partial_final_marker_stops_instead_of_falling_back)
{
	uint8_t out[8];
	uint8_t old_magic[4];
	entelea_storage_audit_entry_t e = {.value_u = 42};

	inj_begin();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"OLD", 3), ENTELEA_OK);
	inj_tear(4, 2);
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW", 3), ENTELEA_E_IO);
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), ENTELEA_E_IO);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 0,
						       old_magic, sizeof(old_magic)),
		      ENTELEA_OK);
	zassert_equal(old_magic[0], 0x31, "previous record not erased");
	inj_end();

	inj_begin();
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	inj_tear(3, 2);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_E_IO);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_E_IO);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_AUDIT, 0,
						       old_magic, sizeof(old_magic)),
		      ENTELEA_OK);
	zassert_equal(old_magic[0], 0x31, "previous audit record not erased");
	inj_end();
}

ZTEST(framework_store, test_every_compaction_write_failure_preserves_source)
{
	for (uint32_t phase = 1; phase <= 9; phase++) {
		uint8_t v;
		uint8_t first[17];

		inj_begin();
		zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"A1", 2),
			      ENTELEA_OK);
		for (v = 0; v < 14; v++) {
			zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK);
		}
		inj_tear(phase, 2);
		v = 99;
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_E_IO,
			      "compaction write %u", (unsigned)phase);
		zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 0,
							       first, sizeof(first)),
			      ENTELEA_OK);
		zassert_equal(first[0], 0x31, "source magic remains");
		zassert_mem_equal(first + 14, "A1", 2, "source data remains");
		entelea_kv_test_reset();
		if (phase == 4 || phase == 8) {
			zassert_equal(entelea_kv_init(), ENTELEA_E_IO,
				      "partial final marker is ambiguous");
		} else {
			kv_expect(APP1, "A1", 2);
			v = 13;
			kv_expect(APP2, &v, 1);
		}
		inj_end();
	}
}

ZTEST(framework_store, test_compaction_preserves_unique_target_record)
{
	uint8_t protected_slot[264], protected_tag[8], legacy[264], after[264], out[4];
	uint8_t value = 42;

	inj_begin();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"KEEP", 4), ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 0,
						       protected_slot, sizeof(protected_slot)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 3960,
						       protected_tag, sizeof(protected_tag)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 4096,
							protected_slot, sizeof(protected_slot)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 4096 + 3960,
							protected_tag, sizeof(protected_tag)),
		      ENTELEA_OK);
	zassert_equal(entelea_store_backend.erase_off(ENTELEA_PART_KV, 0, 4096),
		      ENTELEA_OK);
	for (uint32_t i = 0; i < 15; i++) {
		memset(legacy, 0xFF, sizeof(legacy));
		legacy[0] = 0x31; legacy[1] = 0x4B; legacy[2] = 0x53; legacy[3] = 0x54;
		legacy[4] = 0x02; legacy[5] = 0xA0; legacy[6] = 0; legacy[7] = 0;
		legacy[8] = (uint8_t)(i + 2); legacy[9] = legacy[10] = legacy[11] = 0;
		legacy[12] = 1; legacy[13] = 0; legacy[14] = (uint8_t)i;
		zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, i * 264,
								legacy, sizeof(legacy)),
			      ENTELEA_OK);
	}
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), 4,
		      "unique target record is readable before compaction");
	zassert_mem_equal(out, "KEEP", 4);
	inj_tear(1, 2);
	entelea_result_t r = entelea_kv_save(APP2, &value, 1);

	zassert_not_equal(r, ENTELEA_OK, "unsafe compaction must not succeed");
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), 4,
		      "unique target record must survive interrupted compaction");
	zassert_mem_equal(out, "KEEP", 4);
	zassert_equal(r, ENTELEA_E_STATE, "unsafe layout requires explicit recovery");
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 4096,
						       after, sizeof(after)),
		      ENTELEA_OK);
	zassert_mem_equal(after, protected_slot, sizeof(after), "target was not erased");
	inj_end();
}

ZTEST(framework_store, test_compaction_accepts_duplicate_target_record)
{
	uint8_t protected_slot[264], protected_tag[8], out[4], value;

	inj_begin();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"KEEP", 4), ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 0,
						       protected_slot, sizeof(protected_slot)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 3960,
						       protected_tag, sizeof(protected_tag)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 4096,
							protected_slot, sizeof(protected_slot)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 4096 + 3960,
							protected_tag, sizeof(protected_tag)),
		      ENTELEA_OK);
	for (value = 0; value < 14; value++) {
		zassert_equal(entelea_kv_save(APP2, &value, 1), ENTELEA_OK);
	}
	entelea_kv_test_reset();
	value = 42;
	zassert_equal(entelea_kv_save(APP2, &value, 1), ENTELEA_OK,
		      "identical source copy permits safe compaction");
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), 4);
	zassert_mem_equal(out, "KEEP", 4);
	kv_expect(APP2, &value, 1);
	inj_end();
}

ZTEST(framework_store, test_kv_conflicting_equal_sequence_fails_closed)
{
	uint8_t protected_slot[264], protected_tag[8], legacy[264], after[264], out[8];
	uint8_t value;

	inj_begin();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"KEEP", 4), ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 0,
						       protected_slot, sizeof(protected_slot)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 3960,
						       protected_tag, sizeof(protected_tag)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 4096,
							protected_slot, sizeof(protected_slot)),
		      ENTELEA_OK);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 4096 + 3960,
							protected_tag, sizeof(protected_tag)),
		      ENTELEA_OK);
	zassert_equal(entelea_store_backend.erase_off(ENTELEA_PART_KV, 0, 4096),
		      ENTELEA_OK);
	entelea_kv_test_reset();
	for (value = 0; value < 14; value++) {
		zassert_equal(entelea_kv_save(APP2, &value, 1), ENTELEA_OK);
	}
	memset(legacy, 0xFF, sizeof(legacy));
	legacy[0] = 0x31; legacy[1] = 0x4B; legacy[2] = 0x53; legacy[3] = 0x54;
	legacy[4] = 0x01; legacy[5] = 0xA0; legacy[6] = 0; legacy[7] = 0;
	legacy[8] = 1; legacy[9] = legacy[10] = legacy[11] = 0;
	legacy[12] = 4; legacy[13] = 0;
	memcpy(legacy + 14, "DIFF", 4);
	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_KV, 0,
							legacy, sizeof(legacy)),
		      ENTELEA_OK);
	entelea_kv_test_reset();
	zassert_equal(entelea_kv_load(APP1, out, sizeof(out)), ENTELEA_E_IO,
		      "conflicting records with the same sequence are ambiguous");
	value = 42;
	zassert_equal(entelea_kv_save(APP2, &value, 1), ENTELEA_E_IO,
		      "compaction must not choose one conflicting copy");
	zassert_equal(entelea_storage_test_backend_read(ENTELEA_PART_KV, 0,
						       after, sizeof(after)),
		      ENTELEA_OK);
	zassert_mem_equal(after, legacy, sizeof(after), "target was not erased");
	inj_end();
}

/* P1-5：满页复位后续写 + 压实。旧实现：复位时把满页的对页当空页续写
 * （二次编程），压实先擦目标页再读其上的最新记录（他 app 丢失）。 */
ZTEST(framework_store, test_kv_full_page_reboot_keeps_all)
{
	inj_begin();
	uint8_t v;

	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"A1", 2), ENTELEA_OK);
	for (v = 0; v < 14; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK);
	}
	entelea_kv_test_reset(); /* 页 0 满时复位 */
	for (v = 100; v < 116; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "v=%u", v);
	}
	kv_expect(APP1, "A1", 2);
	v = 115;
	kv_expect(APP2, &v, 1);

	/* 每次保存后复位：覆盖全部游标相位（含满页、压实后、对页残留）。 */
	for (int k = 0; k < 2 * 15 + 2; k++) {
		v = (uint8_t)(150 + k);
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "k=%d", k);
		entelea_kv_test_reset();
		kv_expect(APP1, "A1", 2);
		kv_expect(APP2, &v, 1);
	}
	inj_end();
}

/* P1-6：半写记录不得被接受，失败槽不得重写。旧实现单次整槽写（magic
 * 先落即"有效"），失败后同槽重写。 */
ZTEST(framework_store, test_kv_torn_write_rejected)
{
	inj_begin();
	uint8_t v = 7;

	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"OLD", 3), ENTELEA_OK);
	inj_tear(1, 16); /* 撕裂记录体 */
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW1", 4),
		      ENTELEA_E_IO);
	kv_expect(APP1, "OLD", 3);
	entelea_kv_test_reset();
	kv_expect(APP1, "OLD", 3);

	inj_tear(2, 2); /* 撕裂校验侧带 */
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW2", 4),
		      ENTELEA_E_IO);
	kv_expect(APP1, "OLD", 3);
	inj_tear(3, 2); /* 撕裂原槽提交标记：侧带最终标记尚未写入 */
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW2", 4),
		      ENTELEA_E_IO);
	kv_expect(APP1, "OLD", 3);
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "失败后续写不复用坏槽");
	entelea_kv_test_reset();
	kv_expect(APP1, "OLD", 3);
	kv_expect(APP2, &v, 1);

	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"NEW3", 4), ENTELEA_OK);
	entelea_kv_test_reset();
	kv_expect(APP1, "NEW3", 4);
	inj_end();
}

/* P1-6：读错误≠空/未存；扫描失败不得盲写；压实读错误在擦除前中止。 */
ZTEST(framework_store, test_kv_read_error_propagates)
{
	inj_begin();
	uint8_t b[8];
	uint8_t v;

	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"OLD", 3), ENTELEA_OK);
	entelea_kv_test_reset();
	inj.read_fail = true;
	int32_t r = entelea_kv_load(APP1, b, sizeof(b));

	zassert_true(r < 0 && r != (int32_t)ENTELEA_E_NOTFOUND, "r=%d", r);
	zassert_not_equal(entelea_kv_save(APP1, (const uint8_t *)"BAD", 3),
			  ENTELEA_OK, "扫描失败不得盲写");
	inj.read_fail = false;
	kv_expect(APP1, "OLD", 3); /* 恢复后重试初始化 */

	inj.read_fail = true;
	zassert_equal(entelea_kv_load(APP1, b, sizeof(b)), (int32_t)ENTELEA_E_IO);
	inj.read_fail = false;

	for (v = 0; v < 14; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK);
	}
	inj.read_fail = true; /* 页满：本次保存需压实 */
	v = 99;
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_E_IO);
	inj.read_fail = false;
	kv_expect(APP1, "OLD", 3);
	v = 13;
	kv_expect(APP2, &v, 1);
	v = 99;
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "恢复后压实重试");
	entelea_kv_test_reset();
	kv_expect(APP1, "OLD", 3);
	kv_expect(APP2, &v, 1);
	inj_end();
}

/* P1-5/P1-6：压实复制撕裂 → 源页仍权威、无垃圾被读出；重试与后续
 * 多轮压实不丢数据；副本写完而新记录失败（两页最大 seq 相等）→ 复位
 * 取已用较少页，任一页皆完整。 */
ZTEST(framework_store, test_kv_compaction_tear_keeps_data)
{
	inj_begin();
	uint8_t v;

	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"A1", 2), ENTELEA_OK);
	for (v = 0; v < 14; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK);
	}
	inj_tear(5, 20); /* app1 完整复制后，app2 的体撕裂 */
	v = 50;
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_E_IO);
	kv_expect(APP1, "A1", 2);
	v = 13;
	kv_expect(APP2, &v, 1);
	entelea_kv_test_reset();
	kv_expect(APP1, "A1", 2);
	kv_expect(APP2, &v, 1);

	v = 50;
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "重试");
	kv_expect(APP1, "A1", 2);
	kv_expect(APP2, &v, 1);
	for (v = 60; v < 90; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "v=%u", v);
	}
	entelea_kv_test_reset();
	kv_expect(APP1, "A1", 2);
	v = 89;
	kv_expect(APP2, &v, 1);

	/* 平局：重置为满页（APP1 + 14×APP2），压实复制两槽后
	 * 新记录体（第 9 次写）撕裂。 */
	inj_end();
	inj_begin();
	zassert_equal(entelea_kv_save(APP1, (const uint8_t *)"A1", 2), ENTELEA_OK);
	for (v = 0; v < 14; v++) {
		zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK);
	}
	inj_tear(9, 20);
	v = 70;
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_E_IO);
	v = 13;
	kv_expect(APP2, &v, 1);
	entelea_kv_test_reset();
	kv_expect(APP1, "A1", 2);
	kv_expect(APP2, &v, 1);
	v = 71;
	zassert_equal(entelea_kv_save(APP2, &v, 1), ENTELEA_OK, "平局后续写");
	entelea_kv_test_reset();
	kv_expect(APP1, "A1", 2);
	kv_expect(APP2, &v, 1);
	inj_end();
}

/* P1-6：审计半写不可见、坏槽不复用；读错误使 init/read 显式失败，
 * 不当空环（旧实现：读失败按空环→下次追加擦页毁数据）。 */
ZTEST(framework_store, test_audit_torn_and_read_error)
{
	inj_begin();
	entelea_storage_audit_entry_t e = {.ch_idx = 3, .res = -4, .value_u = 9};
	entelea_storage_audit_entry_t out[8];
	int32_t n;

	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	inj_tear(1, 8);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_E_IO);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	n = (int32_t)entelea_storage_audit_read(out, 8);
	zassert_equal(n, 1, "半写记录不可见");
	zassert_equal(out[0].seq, 1);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	n = (int32_t)entelea_storage_audit_read(out, 8);
	zassert_equal(n, 2);
	zassert_equal(out[1].seq, 2);

	inj_tear(2, 2); /* 撕裂提交标记 */
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_E_IO);
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	n = (int32_t)entelea_storage_audit_read(out, 8);
	zassert_equal(n, 2, "标记半写不可见");

	inj.read_fail = true;
	zassert_not_equal(entelea_storage_audit_init(), ENTELEA_OK, "读错误≠空环");
	n = (int32_t)entelea_storage_audit_read(out, 8);
	zassert_true(n < 0, "n=%d", n);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_E_STATE,
		      "未就绪不写（不擦页）");
	inj.read_fail = false;
	zassert_equal(entelea_storage_audit_init(), ENTELEA_OK);
	zassert_equal(entelea_storage_audit_append(&e, 0), ENTELEA_OK);
	n = (int32_t)entelea_storage_audit_read(out, 8);
	zassert_equal(n, 3);
	zassert_equal(out[0].seq, 1);
	zassert_equal(out[1].seq, 2);
	zassert_equal(out[2].seq, 3, "重扫后 seq=有效最大+1（坏槽不可见、不复用）");
	inj_end();
}

/* RF1 #9（G4）：meta_read 须按调用方容量收窄——活动记录长于目标缓冲 =
 * 显式拒绝且零写出（原 API 无容量参数：按记录长 memcpy，越界写调用方栈）。 */
ZTEST(framework_store, test_rf1_meta_read_respects_capacity)
{
	struct {
		uint8_t small[16];
		uint8_t guard[48];
	} g;
	uint8_t w[40];
	uint16_t len = 0xBEEF;

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	memset(w, 0xC3, sizeof(w));
	memset(&g, 0x5A, sizeof(g));
	zassert_equal(entelea_storage_meta_write(w, sizeof(w)), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(g.small, sizeof(g.small), &len),
		      ENTELEA_E_RANGE, "记录长于容量 = 显式拒绝");
	for (size_t i = 0; i < sizeof(g.guard); i++) {
		zassert_equal(g.guard[i], 0x5A, "越界写出 guard[%u]", (unsigned)i);
	}
	for (size_t i = 0; i < sizeof(g.small); i++) {
		zassert_equal(g.small[i], 0x5A, "拒绝不截断写出 small[%u]",
			      (unsigned)i);
	}
	zassert_equal(len, 0xBEEF, "拒绝不改写 len");
	zassert_equal(entelea_storage_meta_read(g.small, 0, &len), ENTELEA_E_RANGE);

	uint8_t exact[sizeof(w)];

	zassert_equal(entelea_storage_meta_read(exact, sizeof(exact), &len),
		      ENTELEA_OK, "恰等容量可读");
	zassert_equal(len, sizeof(w));
	zassert_mem_equal(exact, w, sizeof(w));
}

ZTEST_SUITE(framework_store, NULL, store_setup, NULL, store_after, NULL);
