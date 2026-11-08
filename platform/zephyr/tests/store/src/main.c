/* SPDX-License-Identifier: Apache-2.0 */
/* R03 存储子闭环 1 测试（oracle 迁自旧 firmware/tests/store/src/main.c 的
 * meta/noinit 用例 + 新故障类别负测：越界/回绕/双损/CRC/one-shot/超长/
 * system_fail 留痕接线）。RAM 后端 = 测试隔离面，不冒充持久介质。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"

static void *store_setup(void)
{
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

/* ---- 启动表：storage_init 尾部追加 ---- */
ZTEST(framework_store, test_boot_table_has_storage_step)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	zassert_equal(ENTELEA_PLATFORM_BOOT_STEP_COUNT, 5);
	zassert_ok(strcmp(entelea_platform_boot_steps[4].name, "storage_init"));
	zassert_not_null(entelea_platform_boot_steps[4].fn);
}

ZTEST_SUITE(framework_store, NULL, store_setup, NULL, NULL, NULL);
