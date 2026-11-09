/* SPDX-License-Identifier: Apache-2.0 */
/* D-3 最小工厂闭环硬件闸（D 批）：Forge（OpenHands SDK 产 wasm →
 * tsap_pack.py 测试根签名）→ 设备（stage → verify → digest 对拍 →
 * factory_intake → PENDING → confirm → ACTIVE → wasm 执行 → 真死循环
 * 有界停止 R-V1 夹具）。产物字节经构建期注入（factory_pkg.h——USB
 * 烧录交付路径；真网交付随 F 批）。
 * 单相位：闸链全绿后 trial_end+flush 清槽收尾（无人值守安全常驻态
 * ——同 hw_flash_persist P3 纪律）；任一环节失败 → system_fail。
 * trial 跨复位持久面已由 hw_flash_persist A-4 证明，本闸不重复。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/package.h>
#include <entelea/prov.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wasm_host.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "factory_pkg.h"

#define CHK(cond, msg) do { \
	bool ok_ = (cond); \
	printk("[hw-fact] %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
	if (!ok_) { \
		goto fail; \
	} \
} while (0)

static entelea_clock_t platform_clock;

int main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-fact] factory loop gate begins (tsap %uB)\n",
	       (uint32_t)FACTORY_TSAP_LEN);
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);

	bool ok = true;

	/* 0) 工厂 provisioning：维护通道注入工厂根 pk0（prov CBOR v1
	 *    最小 9 对——真实产线=烧录窗注入，闸走同一 API 面：校验
	 *    先行 + per-boot 一次窗）。pk1=零（不可伪造）。*/
	static const uint8_t FACTORY_ROOT_PUB[32] = {
		0xf0, 0x84, 0xa4, 0xe5, 0x6f, 0x68, 0xb6, 0xab,
		0x5c, 0xbe, 0xe8, 0x60, 0x3c, 0x59, 0xa4, 0x2a,
		0xc2, 0xef, 0x26, 0x51, 0xc4, 0x93, 0x94, 0xa7,
		0x03, 0x5c, 0xd5, 0xdb, 0x52, 0xdb, 0x47, 0x7a,
	};
	{
		static uint8_t pcb[192];
		uint32_t n = 0;

		pcb[n++] = 0xA9; /* map(9)——prov CBOR v1 最小集 */
		#define PUT(lit) do { \
			if (n + sizeof(lit) - 1 <= sizeof(pcb)) { \
				memcpy(pcb + n, lit, sizeof(lit) - 1); \
			} \
			n += sizeof(lit) - 1; \
		} while (0)  /* 溢出跳写+n 继续增长——末尾统一护栏拒 */
		PUT("\x61" "v");      pcb[n++] = 0x01;          /* v:1 */
		PUT("\x67" "node_id"); PUT("\x65" "node1");
		PUT("\x67" "cube_id"); PUT("\x65" "cube1");
		PUT("\x67" "routers"); PUT("\x81"); PUT("\x6a" "tcp/f:7447");
		PUT("\x63" "pk0");    pcb[n++] = 0x58; pcb[n++] = 32;
		memcpy(pcb + n, FACTORY_ROOT_PUB, 32); n += 32;
		PUT("\x63" "pk1");    pcb[n++] = 0x58; pcb[n++] = 32;
		n += 32; /* pcb 零初始化——pk1=零根（不可伪造） */
		PUT("\x64" "cred");   PUT("\x62" "ok");
		PUT("\x66" "pwr_ma"); PUT("\x19\x01\xf4");   /* 500mA */
		PUT("\x65" "estop");    pcb[n++] = 0x01;
		#undef PUT
		CHK(n <= sizeof(pcb) &&  /* 组装护栏（review P3-3） */
			    entelea_prov_write_maint(pcb, n) == ENTELEA_OK &&
			    entelea_prov_load() == ENTELEA_OK &&
			    memcmp(entelea_prov()->root_pubkeys[0],
				   FACTORY_ROOT_PUB, 32) == 0,
		    "factory provisioning pk0 (maint channel)");
	}

	/* 1) 分步暂存工厂包（staging 链生产 API）。 */
	ok &= entelea_pkg_stage_begin(0) == ENTELEA_OK;
	uint32_t off = 0;

	while (ok && off < FACTORY_TSAP_LEN) {
		uint32_t n = FACTORY_TSAP_LEN - off;

		if (n > 512) {
			n = 512;
		}
		if (entelea_pkg_stage_chunk(0, off, FACTORY_TSAP + off, n) !=
		    ENTELEA_OK) {
			ok = false;
			break;
		}
		off += n;
	}
	CHK(ok, "stage factory package (begin+chunks)");

	/* 2) 验签（显式工厂根=prov pk0 同源；比默认信任集更精确：设备
	 *    验证"包由 Forge 声明的工厂根签名"）。 */
	entelea_tsap_view_t v;

	CHK(entelea_pkg_verify(0, FACTORY_ROOT_PUB, &v, NULL) == ENTELEA_OK &&
		    v.wasm_len > 0 && v.manifest_len > 0,
	    "verify signature (explicit factory root)");

	/* 3) Forge/设备字节对拍（D-4 设备半边）：slot 内容 sha256 ==
	 *    Forge candidate.package_digest（构建注入）。 */
	{
		static uint8_t rd[2048];
		uint32_t st[8];
		uint8_t dg[32];

		entelea_sha256_stream_init(st);
		uint32_t got = 0;

		while (got < FACTORY_TSAP_LEN) {
			uint32_t n = FACTORY_TSAP_LEN - got;

			if (n > sizeof(rd)) {
				n = sizeof(rd);
			}
			if (entelea_pkg_slot_read(0, got, rd, n) != ENTELEA_OK) {
				ok = false;
				break;
			}
			entelea_sha256_stream_update(st, rd, n);
			got += n;
		}
		entelea_sha256_stream_final(st, dg);
		if (memcmp(dg, FACTORY_DIGEST, 32) != 0) {
			printk("[hw-fact] dev=%02x%02x%02x%02x forge=%02x%02x%02x%02x\n",
			       dg[0], dg[1], dg[2], dg[3], FACTORY_DIGEST[0],
			       FACTORY_DIGEST[1], FACTORY_DIGEST[2],
			       FACTORY_DIGEST[3]);
		}
		CHK(ok && memcmp(dg, FACTORY_DIGEST, 32) == 0,
		    "slot bytes == forge digest (D-4 device side)");
	}

	/* 4) 工厂 intake → PENDING_CONFIRM（元数据经构建注入=Forge 侧
	 *    声明；digest/permissions/version 即用户确认对象）。 */
	{
		entelea_factory_candidate_t fc;

		memset(&fc, 0, sizeof(fc));
		memcpy(fc.package_digest, FACTORY_DIGEST, 32);
		fc.permissions = FACTORY_PERMISSIONS;
		fc.runtime_version = FACTORY_VERSION;
		fc.payload_kind = FACTORY_KIND;
		fc.budget_steps = FACTORY_BUDGET;
		fc.mem_quota_kb = FACTORY_MEM_KB;
		strncpy(fc.description, FACTORY_DESC,
			sizeof(fc.description) - 1);
		CHK(entelea_factory_intake(&fc) == ENTELEA_OK &&
			    entelea_trial_state() ==
				    ENTELEA_TRIAL_PENDING_CONFIRM,
		    "factory intake -> PENDING_CONFIRM");
	}

	/* 5) 用户确认（唯一晋升路径）→ ACTIVE。 */
	CHK(entelea_trial_confirm() == ENTELEA_OK &&
		    entelea_trial_state() == ENTELEA_TRIAL_ACTIVE,
	    "trial confirm -> ACTIVE");

	/* 6) 执行工厂产物：load + run（入参 0 → 2*0+1=1）+ spin（0 次空转
	 *    → 0）。principal=空（无导入模块——natives 不触发）。 */
	uint32_t app = 0;
	entelea_result_t wl = entelea_wasm_init() == ENTELEA_OK
				     ? entelea_wasm_load(0, &app)
				     : ENTELEA_E_IO;

	printk("[hw-fact] wasm load rc=%d app=%u\n", (int)wl, app);
	CHK(wl == ENTELEA_OK && app == 1, "wasm load (verified slot, app=1)");
	entelea_principal_t ctx = {0};
	int32_t ret = -1;

	CHK(entelea_wasm_call(app, ctx, "run", 2000, &ret) == ENTELEA_OK &&
		    ret == 1,
	    "call run -> 2*0+1 == 1 (sdk artifact executes)");
	ret = -1;
	{
		entelea_result_t sr = entelea_wasm_call(app, ctx, "spin",
						       2000, &ret);

		printk("[hw-fact] spin rc=%d ret=%d\n", (int)sr, ret);
		CHK(sr == ENTELEA_OK && ret == 0, "call spin -> 0");
	}

	/* 7) 真死循环有界停止（R-V1 夹具：hang 无条件循环，步预算到期 →
	 *    E_TIMEOUT + 实例销毁——替代 wasm 测试"预算 0"技巧证据）。 */
	CHK(entelea_wasm_call(app, ctx, "hang", 50, &ret) ==
		    ENTELEA_E_TIMEOUT &&
		    entelea_wasm_instance_count() == 0,
	    "call hang (real infinite loop) -> E_TIMEOUT bounded stop");

	/* 8) 收尾：结束试用 + 槽清空 + 落盘（安全常驻态）。 */
	CHK(entelea_trial_end() == ENTELEA_OK &&
		    entelea_trial_state() == ENTELEA_TRIAL_NONE &&
		    entelea_trial_flush() == ENTELEA_OK,
	    "trial end + flush (safe steady state)");
	printk("[hw-fact] PASS: factory loop closed (sdk->forge->device)\n");
	return 0;

fail:
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	return 0;
}
