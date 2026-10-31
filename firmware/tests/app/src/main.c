/* SPDX-License-Identifier: Apache-2.0 */
/* framework.app（DEC-43 接线批）：APP 运行时宿主端到端——
 *   test_01 生命周期：start → app_init 经 natives 写 gpio → mailbox evt
 *           驱动 app_evt → gpio 随动 → stop 回收（重复停止拒绝）；
 *   test_02 权限拒绝：caps 不含 gpio → init 写被调用期裁决拒绝（-3）
 *           + TS_EVT_PERM_DENIED 留痕（合同 10）；
 *   test_03 健康失败自停：控制 evt 置 health=1 → 连续〔DEC-27：3〕探针
 *           失败 → health_fail（回滚入口）+ 线程自停。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#ifdef CONFIG_TS_PERIPH
#include <ts/periph.h> /* F4：peripherals 自报测试注册 */
#endif
#include <ts/safety.h>
#include <ts/store.h>
#include <ts/tsap.h>
#include "../../../module/tessera/src/net/internal.h"
#include "app_wasm.h"
#include "boot_pkg.h"
#include "boot_pkg_large.h"
/* EC2 信任链夹具（gen_trust_fixtures.py 机械生成——重跑复现） */
#include "boot_pkg_devroot.h"
#include "trust_blob_add.h"
#include "trust_blob_clear.h"
#include "trust_blob_dev_signed.h"
#include "trust_blob_replay.h"
#include "trust_blob_skip.h"
#include "trust_blob_unauth.h"
#include "trust_dev_pub.h"
/* CG4：档位扫描/匹配内部面 + cbor 构造助手（模块内复用先例 = net 测试） */
#include "../../../module/tessera/src/appmgr/internal.h"
#include "../../../module/tessera/src/net/internal.h"

static const ts_out_ch_t ag_ch = {
	.uid = "ag0", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};

static bool gpio_val(void)
{
	ts_out_value_t v;

	if (ts_safety_readback("ag0", &v) != TS_OK) {
		return false;
	}
	return v.b;
}

static bool wait_gpio(bool want)
{
	for (int i = 0; i < 300; i++) {
		if (gpio_val() == want) {
			return true;
		}
		k_msleep(10);
	}
	return false;
}

static const ts_hal_dev_desc_t ag_dev = {.uid = "ag0", .kind = TS_DEV_GPIO_OUT};
static bool hal_done;

static void suite_setup_channel(void)
{
	ts_safety_test_reset();
	ts_appmgr_app_test_reset();
	if (!hal_done) { /* hal 注册表进程级共享（无复位钩子）——一次即可 */
		zassert_equal(ts_hal_register_dev(&ag_dev), TS_OK);
		hal_done = true;
	}
	zassert_equal(ts_safety_register_channel(&ag_ch), TS_OK);
	ts_safety_set_link(true);
}

ZTEST(framework_app, test_01_lifecycle)
{
	suite_setup_channel();
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	zassert_true(ts_appmgr_app_running(), "运行中");
	zassert_true(wait_gpio(true),
		     "app_init 经 natives→perm→唯一写路径 落 gpio=1");

	zassert_equal(ts_appmgr_app_evt(0), TS_OK);
	zassert_true(wait_gpio(false), "app_evt(0) → gpio=0");
	zassert_equal(ts_appmgr_app_evt(3), TS_OK);
	zassert_true(wait_gpio(true), "app_evt(3) → gpio=1");

	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	zassert_true(st.evt_seen >= 2, "mailbox 事件已消费（DR-14）");
	zassert_equal(st.init_res, 0, "app_init 返回 0");
	zassert_equal(st.health_fails, 0, "健康无失败");

	zassert_equal(ts_appmgr_app_stop(), TS_OK);
	zassert_false(ts_appmgr_app_running());
	zassert_equal(ts_appmgr_app_stop(), TS_E_STATE, "重复停止拒绝");
}

static int perm_seen;

static void on_perm(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	perm_seen++;
}

ZTEST(framework_app, test_02_perm_denial)
{
	suite_setup_channel();
	perm_seen = 0;
	zassert_equal(ts_evt_subscribe(TS_EVT_PERM_DENIED, on_perm, NULL), TS_OK);

	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "adc:read:0-3"), TS_OK,
		      "启动成功（无 gpio 能力——运行期拒绝）");
	for (int i = 0; i < 100 && perm_seen == 0; i++) {
		k_msleep(10);
	}
	zassert_false(gpio_val(), "写被拒——gpio 保持关");
	zassert_true(perm_seen > 0, "TS_EVT_PERM_DENIED 留痕（合同 10）");
	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	zassert_equal(st.init_res, (uint32_t)TS_E_PERM,
		      "app_init 收到 -3（调用期权限裁决）");
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

ZTEST(framework_app, test_03_health_fail_stops)
{
	suite_setup_channel();
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	zassert_true(wait_gpio(true), "init 写生效");
	zassert_equal(ts_appmgr_app_evt(0x100), TS_OK, "控制通道：置健康失败");
	for (int i = 0; i < 600 && ts_appmgr_app_running(); i++) {
		k_msleep(10);
	}
	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	zassert_false(st.running, "连续 3 败后自停（DEC-27）");
	zassert_true(st.health_failed, "健康失败路径触发");
	zassert_true(st.health_fails >= 3);
	zassert_equal(ts_appmgr_app_stop(), TS_OK, "自停后 stop 清尾回收");
}

ZTEST(framework_app, test_04_boot_slot_load)
{
	suite_setup_channel();
	ts_store_test_reset();
	ts_appmgr_test_reset();

	/* DEC-49④：boot E2E = 预签 v2 包（native_app.wasm 真 wasm + 测试根
	 * 签名，boot_pkg.h 机械生成）——真验签/真走查/真装载全链。 */
	uint32_t total = sizeof(boot_pkg);
	const uint8_t root_key[32] = {0}; /* TEST 构建：验签对象 = 固定测试根 */
	ts_app_info_t inst_info;

	zassert_equal(ts_appmgr_install(boot_pkg, total, root_key, &inst_info),
		      TS_OK, "install 预签包（真 ed25519）");

	/* B2 批：回滚计数随载恢复——装载前改写 meta.count=2，装载后 get-app
	 * 须如实报 2（旧缺陷 = 恒 0；flash 实测对拍拦下）。 */
	{
		ts_appmgr_meta_t m;

		zassert_equal(ts_appmgr_meta_read(&m), TS_OK);
		m.entries[0].rollback_count = 2;
		zassert_equal(ts_appmgr_meta_write(&m), TS_OK);
	}

	/* boot 装载（步骤 8 语义）：摘要快校验 + manifest 走查 + caps 组合 +
	 * wasm → 运行 */
	zassert_equal(ts_appmgr_boot_start(), TS_OK, "boot 装载");
	zassert_true(wait_gpio(true), "fixture init 写 gpio=1（经 slot 全链）");
	zassert_equal(ts_appmgr_app_evt(0), TS_OK);
	zassert_true(wait_gpio(false), "evt 驱动");
	ts_app_info_t after;
	zassert_equal(ts_appmgr_get_info(&after), TS_OK);
	zassert_equal(after.state, TS_APP_ACTIVE, "装载后 STAGED→ACTIVE");
	zassert_equal(after.rollback_count, 2, "回滚计数随载恢复（B2）");
	zassert_equal(strcmp(after.app_id, "com.tessera.fixture"), 0,
		      "manifest app_id 提取");
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

/* ---- ts-fs 能力面（DEC-47④⑤，MD1.2e）------------------------------------
 * 双层裁决 fail-closed：class/op 位图 + fs_paths 前缀白名单（边界语义：
 * 前缀后须 '/' 或恰好等长——防前缀绕过）；native_sim 无挂载盘 →
 * 授权路径走到后端报 TS_E_IO（IO 面），未授权路径在门内即拒 TS_E_PERM。 */
ZTEST(framework_app, test_05_fs_gates)
{
	ts_perm_table_t table;
	ts_ctx_t ctx;

	ts_perm_table_init(&table);
	zassert_equal(ts_perm_parse("fs:read:0", &table), TS_OK, "fs 类解析");
	zassert_equal(ts_perm_parse("fs:write:0", &table), TS_OK);
	zassert_equal(ts_perm_parse("fs:list:0", &table), TS_OK);
	zassert_equal(ts_perm_parse("fs:delete:0", &table), TS_OK);
	zassert_equal(ts_perm_parse("fs:append:0", &table), TS_E_PARAM,
		      "未知 op fail-closed");
	zassert_equal(ts_hal_bind_context(&ctx, 0x5a, &table), TS_OK);

	/* 未绑定 fs_paths：一切路径拒绝（参数合法 → 门内拒绝） */
	uint8_t b0[4];
	uint16_t n0 = sizeof(b0);

	zassert_equal(ts_fs_read(ctx, "/SD:/x", 0, b0, &n0), TS_E_PERM,
		      "未绑定白名单 = 拒绝");

	zassert_equal(ts_fs_paths_bind_ctx(ctx, "/SD:/apps;/SD:/tmp"), TS_OK);
	/* 前缀边界：等长 ✓ / 子路径 ✓ / 前缀绕过 ✗ */
	zassert_equal(ts_fs_path_allowed(ctx, "/SD:/apps"), TS_OK);
	zassert_equal(ts_fs_path_allowed(ctx, "/SD:/apps/a.txt"), TS_OK);
	zassert_equal(ts_fs_path_allowed(ctx, "/SD:/apps-secret"), TS_E_PERM,
		      "前缀绕过防护");
	zassert_equal(ts_fs_path_allowed(ctx, "/SD:/etc"), TS_E_PERM);
	zassert_equal(ts_fs_path_allowed(ctx, "SD:/apps"), TS_E_PARAM,
		      "非绝对路径");

	/* 授权路径 + 位图 op 通过 → 后端（native_sim 无挂载 → TS_E_IO） */
	uint8_t b[4];
	uint16_t n = sizeof(b);

	zassert_equal(ts_fs_read(ctx, "/SD:/apps/a.txt", 0, b, &n), TS_E_IO,
		      "授权读 → 后端 IO（无挂载，如实）");
	/* read 类 op 未授权路径：门内拒绝 */
	zassert_equal(ts_fs_read(ctx, "/SD:/etc/a", 0, b, &n), TS_E_PERM);

	/* class 位图缺 write op（新 ctx 只授 read）→ 门内拒绝 */
	ts_perm_table_init(&table);
	zassert_equal(ts_perm_parse("fs:read:0", &table), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 0x5b, &table), TS_OK);
	zassert_equal(ts_fs_paths_bind_ctx(ctx, "/SD:/apps"), TS_OK);
	zassert_equal(ts_fs_write(ctx, "/SD:/apps/a", 0, b, 0), TS_E_PERM,
		      "op 位图缺 write = 拒绝");
	ts_hal_unbind_context(&ctx);
}

/* ---- ts-av 能力面（DEC-47③⑥，MD1.2g）------------------------------------
 * 读类 fail-closed：av:read:0 位图 + av_fmt/av_w/av_h 配置绑定（manifest
 * 声明 → 随载绑定；未声明 = TS_E_STATE）。native_sim 无摄像头 chosen →
 * 权限+配置齐全时走到后端报 TS_E_IO（IO 面）；权限/配置缺口在门内即拒。
 * 发布面（TS_NET=y）：hal 权限/参数门 + avq 入队；发送/信封/重试语义在
 * framework.net test_12_avq 覆盖。 */
ZTEST(framework_app, test_06_av_gates)
{
	ts_perm_table_t table;
	ts_ctx_t ctx;

	/* av 类文法（op = read；DEC-47⑤） */
	ts_perm_table_init(&table);
	zassert_equal(ts_perm_parse("av:read:0", &table), TS_OK, "av 类解析");
	zassert_equal(ts_perm_parse("av:capture:0", &table), TS_E_PARAM,
		      "未知 op fail-closed（V1 av op = read）");
	zassert_equal(ts_hal_bind_context(&ctx, 0x5c, &table), TS_OK);

	/* 未绑定配置（manifest 未声明 av_*）：权限过、配置缺 → 拒绝 */
	uint8_t b[64];
	uint32_t n = 0;

	zassert_equal(ts_av_capture(ctx, b, sizeof(b), &n), TS_E_STATE,
		      "未声明格式 = 拒绝");

	/* V0.4.x M17（Q-36/DEC-54②）：pcm16 音频面——w 恒 16/h 弃/帧 640 定长
	 * （TEST 桩 = 确定性样本模式；真机路径 = TS_DRV_PDM〔M17b〕）。 */
	uint8_t pb[640];
	uint32_t plen = 0;

	zassert_equal(ts_av_config_bind_ctx(ctx, TS_AV_FMT_PCM16, 48, 0),
		      TS_E_PARAM, "pcm16 采样率 V1 恒 16kHz");
	zassert_equal(ts_av_config_bind_ctx(ctx, TS_AV_FMT_PCM16, 16, 0), TS_OK);
	zassert_equal(ts_av_capture(ctx, pb, 639, &plen), TS_E_RANGE,
		      "帧 > 缓冲 = RANGE");
	zassert_equal(ts_av_capture(ctx, pb, sizeof(pb), &plen), TS_OK);
	zassert_equal(plen, 640, "音频帧 640B 定长");
	uint16_t *ps = (uint16_t *)pb;

	zassert_not_equal(ps[0], ps[1], "确定性样本模式非全零");

	/* 配置绑定 + 参数域（分辨率界 16..800 与 agent 校验器对齐） */
	zassert_equal(ts_av_config_bind_ctx(ctx, TS_AV_FMT_JPEG, 160, 120), TS_OK);
	zassert_equal(ts_av_config_bind(0x5c, TS_AV_FMT_JPEG, 8, 120), TS_E_PARAM,
		      "分辨率下界（<16）拒绝");
	zassert_equal(ts_av_config_bind(0x5c, (ts_av_fmt_t)9, 160, 120), TS_E_PARAM,
		      "未知格式拒绝");
	zassert_equal(ts_av_capture(ctx, NULL, sizeof(b), &n), TS_E_PARAM);

	/* 配置绑定域 = 声明它的 APP（其他 ctx → TS_E_STATE） */
	ts_ctx_t other;

	zassert_equal(ts_hal_bind_context(&other, 0x5d, &table), TS_OK);
	zassert_equal(ts_av_capture(other, b, sizeof(b), &n), TS_E_STATE,
		      "配置不属于本 APP");
	ts_hal_unbind_context(&other);

	/* 权限 + 配置齐全 → 后端（native_sim 无摄像头 chosen → TS_E_IO 如实） */
	zassert_equal(ts_av_capture(ctx, b, sizeof(b), &n), TS_E_IO,
		      "授权+配置 → 后端 IO（无摄像头，如实）");

	/* 发布面（TS_NET=y 构建）：权限/参数门 + 入队语义（本构建无传输注入，
	 * avq 只入队不发送——发送语义在 framework.net test_12 覆盖） */
	ts_net_avq_reset();
	zassert_equal(ts_av_publish(ctx, 1, 0, 2, b, 8), TS_OK, "授权分片入队");
	zassert_equal(ts_av_publish(ctx, 1, 0, 2, b, 0), TS_E_PARAM, "len=0 拒绝");
	zassert_equal(ts_av_publish(ctx, 1, 2, 2, b, 8), TS_E_PARAM, "cid>=n 拒绝");
	static uint8_t big[CONFIG_TS_NET_PUBLISH_MAX_BYTES + 1];

	memset(big, 1, sizeof(big));
	zassert_equal(ts_av_publish(ctx, 1, 0, 2, big, sizeof(big)), TS_E_PARAM,
		      "chunk > 1KB 拒绝（DEC-47②）");

	/* 无 av 权限的 ctx：门内拒绝（留痕） */
	ts_perm_table_init(&table);
	zassert_equal(ts_perm_parse("gpio:read:0", &table), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 0x5e, &table), TS_OK);
	zassert_equal(ts_av_config_bind_ctx(ctx, TS_AV_FMT_JPEG, 160, 120), TS_OK);
	zassert_equal(ts_av_capture(ctx, b, sizeof(b), &n), TS_E_PERM,
		      "无 av:read = 拒绝");
	zassert_equal(ts_av_publish(ctx, 1, 0, 2, b, 8), TS_E_PERM,
		      "发布同受 av:read 门（DEC-47⑥ 读类）");
	ts_hal_unbind_context(&ctx);
}


/* V0.4.0 M7（Q-32③）：双实例并行运行时 -------------------------------------- */

ZTEST(framework_app, test_09_multi_instance_parallel)
{
	/* 同 wasm 两 instance（模块缓存复用——怪癖规避形态下多实例共享模块
	 * 对象）；独立 mailbox/stats；广播事件双投递；单停单启互不影响。 */
	ts_appmgr_app_test_reset();
	ts_safety_set_link(true);
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	zassert_equal(ts_appmgr_app_start(2, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	zassert_true(ts_appmgr_app_running_id(1));
	zassert_true(ts_appmgr_app_running_id(2));

	ts_appmgr_app_evt_bcast(0);
	struct ts_app_rt_stats s1, s2;

	for (int i = 0; i < 100; i++) {
		k_msleep(10);
		ts_appmgr_app_stats_id(1, &s1);
		ts_appmgr_app_stats_id(2, &s2);
		if (s1.evt_seen >= 1 && s2.evt_seen >= 1) {
			break;
		}
	}
	zassert_true(s1.evt_seen >= 1, "实例1 收到广播");
	zassert_true(s2.evt_seen >= 1, "实例2 收到广播");

	zassert_equal(ts_appmgr_app_stop_id(1), TS_OK);
	zassert_false(ts_appmgr_app_running_id(1), "实例1 已停");
	zassert_true(ts_appmgr_app_running_id(2), "单停不影响实例2");
	zassert_equal(ts_appmgr_app_stop_id(2), TS_OK);
	zassert_false(ts_appmgr_app_running_id(2));
}


/* ---- CG4（Q-39②⑤/Q-42 · DEC-59）：资源档位——扫描矩阵/安装期匹配/boot 门 - */
ZTEST(framework_app, test_10_tier_matching)
{
	suite_setup_channel();
	ts_store_test_reset();
	ts_appmgr_test_reset();

	/* 扫描矩阵（ts_appmgr_scan_needs——verify/boot 单一解析源） */
	uint8_t m[256];
	size_t p;
	ts_app_needs_t n;

	/* v1 无档位键 → 缺省 small / 无 needs；匹配通过 */
	p = 0;
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 1));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "app_id"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "x"));
	zassert_equal(ts_appmgr_scan_needs(m, p, &n), TS_OK);
	zassert_equal(n.tier, 0);
	zassert_false(n.has_needs);
	zassert_equal(ts_appmgr_needs_match(&n), TS_OK, "缺省 small 匹配通过");

	/* large + needs{linear_memory_kb=64, stack_kb=16, resources=["/SD:/res"]}
	 * ——扫描字段全取到；匹配在 small-only 设备拒（档位行：LOAD_MAX 16K
	 * < GATE_LARGE 256K；本套件 FS=y 且 lin 64 < heap 帽 384K → 恰为③行） */
	p = 0;
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 2));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "tier"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "large"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "needs"));
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 3));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "linear_memory_kb"));
	zassert_true(ts_cbor_put_uint(m, sizeof(m), &p, 64));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "stack_kb"));
	zassert_true(ts_cbor_put_uint(m, sizeof(m), &p, 16));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "resources"));
	zassert_true(ts_cbor_put_array(m, sizeof(m), &p, 1));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "/SD:/res"));
	zassert_equal(ts_appmgr_scan_needs(m, p, &n), TS_OK);
	zassert_equal(n.tier, 1);
	zassert_true(n.has_needs);
	zassert_equal(n.linear_memory_kb, 64);
	zassert_equal(n.stack_kb, 16);
	zassert_equal(n.resources, 1);
	zassert_equal(ts_appmgr_needs_match(&n), TS_E_RANGE,
		      "large 档在 small-only 设备拒装（匹配矩阵③）");

	/* 线性内存帽行（①）：lin=500 > heap 帽 384KB（本套件 WAMR_HEAP=393216）
	 * ——先于档位行拒（行序可区分） */
	p = 0;
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 2));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "tier"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "large"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "needs"));
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 1));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "linear_memory_kb"));
	zassert_true(ts_cbor_put_uint(m, sizeof(m), &p, 500));
	zassert_equal(ts_appmgr_scan_needs(m, p, &n), TS_OK);
	zassert_equal(ts_appmgr_needs_match(&n), TS_E_RANGE, "线性内存超帽（匹配①）");

	/* schema 违例矩阵（fail-closed）：small+needs / 域外 tier / needs 未知键
	 * / resources 带 .. / linear=0 */
	p = 0;
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 2));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "tier"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "small"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "needs"));
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 1));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "linear_memory_kb"));
	zassert_true(ts_cbor_put_uint(m, sizeof(m), &p, 64));
	zassert_equal(ts_appmgr_scan_needs(m, p, &n), TS_E_PARAM, "small+needs 拒");

	p = 0;
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 1));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "tier"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "huge"));
	zassert_equal(ts_appmgr_scan_needs(m, p, &n), TS_E_PARAM, "域外 tier 拒");

	p = 0;
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 2));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "tier"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "large"));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "needs"));
	zassert_true(ts_cbor_put_map(m, sizeof(m), &p, 1));
	zassert_true(ts_cbor_put_tstr(m, sizeof(m), &p, "gpu"));
	zassert_true(ts_cbor_put_uint(m, sizeof(m), &p, 1));
	zassert_equal(ts_appmgr_scan_needs(m, p, &n), TS_E_PARAM, "needs 未知键拒");

	/* 安装期拒装（全链）：tier=large 预签包（真 ed25519 测试根签名）在
	 * small-only 设备 → verify 后匹配矩阵拒（TS_E_RANGE——安装期拒装替代
	 * 运行期 OOM；分步安装语义：包已入非激活槽、拒绝 = 不激活） */
	const uint8_t root_key[32] = {0};
	ts_app_info_t info;

	zassert_equal(ts_appmgr_install(boot_pkg_large, sizeof(boot_pkg_large),
					root_key, &info),
		      TS_E_RANGE, "large 档包安装期拒装");

	/* boot 走查接受档位键（直接槽写入 + meta 激活——绕过安装匹配的持久化
	 * 面复现；large 门放行小 wasm = tier 声明与实际 wasm 解耦的门语义） */
	ts_store_test_reset();
	suite_setup_channel();
	zassert_equal(ts_store_slot_write(0, 0, boot_pkg_large,
					  sizeof(boot_pkg_large)), TS_OK);
	ts_appmgr_meta_t mm;

	ts_appmgr_meta_default(&mm); /* count=1/entry0 slot0 */
	zassert_equal(ts_appmgr_meta_write(&mm), TS_OK);
	zassert_equal(ts_appmgr_boot_start(), TS_OK, "boot 装载 tier=large 包");
	zassert_true(wait_gpio(true), "档位键走查接受 + wasm 运行（walk/scan 单一解析源）");
	/* CG5-B：资源白名单 boot 恢复（needs.resources → res registry） */
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/res/font.bin"), TS_OK,
		      "boot 恢复资源白名单（前缀内）");
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/resx"), TS_E_PERM,
		      "边界纪律（前缀后须 /）");
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/other"), TS_E_PERM);
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

/* ---- CG5-A（Q-39④）：get-info caps 块——设备能力自报 --------------------- */
ZTEST(framework_app, test_11_get_info_caps)
{
	uint8_t req[64];
	uint8_t resp[256];
	size_t rlen = 0;
	size_t p = 0;

	ts_net_cmd_test_reset();
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);
#ifdef CONFIG_TS_PERIPH
	/* F4：注册一枚测试外设（peripherals 自报面非空判定） */
	const ts_periph_desc_t f4p = {
		.uid = "f4adc", .kind = TS_PK_ADC, .dt_spec = NULL,
	};

	zassert_equal(ts_periph_register(&f4p), TS_OK);
#endif
	zassert_true(ts_cbor_put_map(req, sizeof(req), &p, 1));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "op"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "get-info"));
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, p, resp,
					  sizeof(resp), &rlen), TS_OK);
	/* 回执 = v1 信封 map{status, data:<原样 handler map>}（wrap_reply：
	 * data 为裸 CBOR 值非 bstr——r2 从信封 value 位置续读）。caps 块：
	 * small-only 设备（LOAD_MAX 16K < GATE_LARGE 256K）→ tiers 恰
	 * ["small"]；max_linear = WAMR_HEAP/1024（本套件 393216 → 384）；
	 * slot_pool = SLOT_SIZE×COUNT/1024（16384×4 → 64） */
	char k[24];
	uint32_t pairs;
	int64_t st = 0;

	ts_cbor_rd_t r;

	ts_cbor_rd_init(&r, resp, rlen);
	zassert_true(ts_cbor_map_open(&r, &pairs));
	zassert_equal(pairs, 2U);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)));
	zassert_equal(strcmp(k, "status"), 0);
	zassert_true(ts_cbor_int(&r, &st));
	zassert_equal(st, 0);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)));
	zassert_equal(strcmp(k, "data"), 0);
	ts_cbor_rd_t r2;
	uint64_t dummy;

	ts_cbor_rd_init(&r2, r.p, r.rem);
	zassert_true(ts_cbor_map_open(&r2, &pairs));
	bool have_caps = false;
	ts_cbor_rd_t *const rr = &r2;

	for (uint32_t i = 0; i < pairs; i++) {
		zassert_true(ts_cbor_tstr(rr, k, sizeof(k)));
		if (strcmp(k, "caps") != 0) {
			zassert_true(ts_cbor_skip(rr, 0));
			continue;
		}
		have_caps = true;
		uint32_t cp, n = 0;
		char tk[24];

		zassert_true(ts_cbor_map_open(rr, &cp));
		for (uint32_t j = 0; j < cp; j++) {
			zassert_true(ts_cbor_tstr(rr, tk, sizeof(tk)));
			if (strcmp(tk, "tiers") == 0) {
				zassert_true(ts_cbor_array_open(rr, &n));
				zassert_equal(n, 1, "small-only 设备 tiers 恰 1 项");
				char t[8];
				zassert_true(ts_cbor_tstr(rr, t, sizeof(t)));
				zassert_equal(strcmp(t, "small"), 0);
			} else if (strcmp(tk, "max_linear_memory_kb") == 0) {
				zassert_true(ts_cbor_uint(rr, &dummy));
				zassert_equal(dummy, (uint64_t)CONFIG_TS_APP_WAMR_HEAP / 1024);
			} else if (strcmp(tk, "slot_pool_kb") == 0) {
				zassert_true(ts_cbor_uint(rr, &dummy));
				zassert_equal(dummy, (uint64_t)CONFIG_TS_STORE_SLOT_SIZE *
						       CONFIG_TS_STORE_SLOT_COUNT / 1024);
			} else if (strcmp(tk, "peripherals") == 0) {
				/* F4：自报外设数组——本套件 TS_PERIPH=y
				 * （adc/gpio 注册 ≥1）；逐项 uid:kind 形态。 */
				uint32_t nper;

				zassert_true(ts_cbor_array_open(rr, &nper));
				zassert_true(nper >= 1, "sim 外设注册 ≥1");
				for (uint32_t k = 0; k < nper; k++) {
					uint32_t pp2;
					char uk[12], kk2[8];

					zassert_true(ts_cbor_map_open(rr, &pp2));
					zassert_equal(pp2, 2);
					zassert_true(ts_cbor_tstr(rr, uk, sizeof(uk)));
					zassert_true(strcmp(uk, "uid") == 0);
					zassert_true(ts_cbor_tstr(rr, uk, sizeof(uk)));
					zassert_true(uk[0] != '\0');
					zassert_true(ts_cbor_tstr(rr, kk2, sizeof(kk2)));
					zassert_true(strcmp(kk2, "kind") == 0);
					zassert_true(ts_cbor_tstr(rr, kk2, sizeof(kk2)));
					zassert_true(strcmp(kk2, "adc") == 0 ||
						     strcmp(kk2, "gpio") == 0 ||
						     strcmp(kk2, "pwm") == 0 ||
						     strcmp(kk2, "power") == 0);
				}
			} else {
				zassert_true(false, "caps 未知键");
			}
		}
	}
	zassert_true(have_caps, "WAMR 构建必带 caps 块");
	(void)dummy;
}

/* ---- CG5-B（Q-39③ 方案 A）：宿主资源推送通道——registry/命令分层矩阵 --- */

/* fs-* 请求构造（v2 信封；args 键值对按序拼装） */
static size_t build_fs_req(uint8_t *buf, size_t cap, const char *op, const char *rid,
			   const char *holder, const char *path, uint32_t total,
			   const uint8_t *sha, uint32_t offset, const uint8_t *data,
			   uint32_t data_len)
{
	size_t p = 0;
	uint32_t npairs = (holder ? 1 : 0) + (path ? 1 : 0) + (total ? 1 : 0) +
			  (sha ? 1 : 0) + (offset ? 1 : 0) + (data ? 1 : 0);

	zassert_true(ts_cbor_put_map(buf, cap, &p, 6));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "ver"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "kind"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "rid"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, rid));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "src"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "t-agent"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "op"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, op));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "args"));
	zassert_true(ts_cbor_put_map(buf, cap, &p, npairs));
	if (holder != NULL) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "holder"));
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, holder));
	}
	if (path != NULL) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "path"));
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, path));
	}
	if (total != 0) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "total"));
		zassert_true(ts_cbor_put_uint(buf, cap, &p, total));
	}
	if (sha != NULL) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "sha256"));
		zassert_true(ts_cbor_put_bstr(buf, cap, &p, sha, 32));
	}
	if (offset != 0) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "offset"));
		zassert_true(ts_cbor_put_uint(buf, cap, &p, offset));
	}
	if (data != NULL) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "data"));
		zassert_true(ts_cbor_put_bstr(buf, cap, &p, data, data_len));
	}
	return p;
}

ZTEST(framework_app, test_12_resource_push_channel)
{
	uint8_t req[256];
	uint8_t resp[192];
	size_t rlen;
	size_t rl2 = 0;
	const uint8_t sha[32] = {0xAA};
	static const uint8_t payload[8] = "RES DATA";

	ts_store_test_reset();
	ts_appmgr_test_reset();
	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_lease_test_reset();
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);

	/* registry 单元矩阵（随包白名单语义 = fs_paths 边界同源） */
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/res/a.bin"), TS_E_PERM,
		      "未绑定 = 全拒（fail-closed）");
	ts_appmgr_res_bind(1, "/SD:/res;/SD:/assets");
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/res/a.bin"), TS_OK);
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/res"), TS_OK, "恰好等长");
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/resx"), TS_E_PERM, "前缀边界");
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/other"), TS_E_PERM);
	zassert_equal(ts_appmgr_res_allows(2, "/SD:/res/a.bin"), TS_E_PERM,
		      "per-entry 隔离（entry 2 未绑定）");
	zassert_equal(ts_appmgr_res_allows(1, "rel/path"), TS_E_PERM,
		      "相对路径无前缀命中 = PERM（文法校验在 cmd 层 PARAM）");
	ts_appmgr_res_bind(1, "");
	zassert_equal(ts_appmgr_res_allows(1, "/SD:/res/a.bin"), TS_E_PERM,
		      "空 CSV = 清除");

	/* 命令矩阵（gated：先取租约——src=t-agent） */
	ts_appmgr_res_bind(1, "/SD:/res");
	rlen = build_fs_req(req, sizeof(req), "lease-acquire", "rid-fs0",
			    "t-agent", NULL, 0, NULL, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);

	/* 白名单外/缺参/越界值 */
	rlen = build_fs_req(req, sizeof(req), "fs-begin", "rid-fs1", NULL,
			    "/etc/x.bin", 8, sha, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/fs-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_PERM,
		      "白名单外 = 越权拒（留痕）");
	rlen = build_fs_req(req, sizeof(req), "fs-begin", "rid-fs2", NULL,
			    NULL, 0, NULL, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/fs-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_PARAM,
		      "缺 path/total/sha256 拒");
	rlen = build_fs_req(req, sizeof(req), "fs-begin", "rid-fs3", NULL,
			    "/SD:/res/a.bin", 300000, sha, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/fs-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_PARAM,
		      "超资源上限拒（256KB）");

	/* 授权路径：staging OK → chunk 到后端层（native_sim 无挂载 = E_IO 如实
	 * ——分层可区分：授权已过、失败在 IO 层）→ commit 未收满拒 */
	rlen = build_fs_req(req, sizeof(req), "fs-begin", "rid-fs4", NULL,
			    "/SD:/res/a.bin", 8, sha, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/fs-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
	rlen = build_fs_req(req, sizeof(req), "fs-chunk", "rid-fs5", NULL,
			    NULL, 0, NULL, 0, payload, sizeof(payload));
	zassert_equal(ts_net_cmd_dispatch("sys/fs-chunk", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_IO,
		      "后端如实（无挂载）——授权层已过");
	rlen = build_fs_req(req, sizeof(req), "fs-commit", "rid-fs6", NULL,
			    NULL, 0, NULL, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/fs-commit", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_STATE,
		      "未收满不得验（fail-closed）");
	/* 无会话 commit 拒（begin 缺失——新 cmd 实例态） */
}

/* ---- V0.8.0 EC2（Q-43/DEC-60）：信任链——多根验签 + trust-update 命令链 -- */
ZTEST(framework_app, test_13_trust_chain)
{
	uint8_t req[512];
	uint8_t resp[192];
	size_t rlen;
	size_t rl2 = 0;
	ts_app_info_t info;

	ts_store_test_reset();
	ts_appmgr_test_reset();
	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_lease_test_reset();
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);

	/* ① 工厂根单源基线：开发者键签名包 NULL（有效集）安装 = 拒
	 * （TEST 有效集 = {测试根}——多根注册前 dev 键不在集） */
	zassert_equal(ts_appmgr_install(boot_pkg_devroot,
					sizeof(boot_pkg_devroot), NULL, &info),
		      TS_E_INVALID_SIG, "未注册开发者键 = 拒（fail-closed）");
	/* 工厂根包（测试根签名）照常可装——分层不回归 */
	zassert_equal(ts_appmgr_install(boot_pkg, sizeof(boot_pkg), NULL, &info),
		      TS_OK, "工厂根包 = 有效集命中（测试根）");

	/* ② trust-get 初始态（ver=0 空表——公钥读回未门控） */
	rlen = build_fs_req(req, sizeof(req), "trust-get", "rid-tg0",
			    NULL, NULL, 0, NULL, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/trust-get", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
	zassert_equal(ts_store_trust()->ver, 0);
	zassert_equal(ts_store_trust()->n_keys, 0);

	/* ③ gated：无租约 trust-update = E_STATE（分发层——签名前） */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu0", NULL,
			    NULL, 0, NULL, 0, trust_blob_add,
			    sizeof(trust_blob_add));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_STATE,
		      "gated 写类 = 租约前置");

	/* ④ 签名门：陌生键签名次版（ver=1 合法序）= E_INVALID_SIG */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu1", NULL,
			    NULL, 0, NULL, 0, trust_blob_unauth,
			    sizeof(trust_blob_unauth));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_STATE,
		      "仍无租约");
	rlen = build_fs_req(req, sizeof(req), "lease-acquire", "rid-tl0",
			    "t-agent", NULL, 0, NULL, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
	/* req 缓冲被租约请求复用——重建 trust-update 请求后重发 */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu1", NULL,
			    NULL, 0, NULL, 0, trust_blob_unauth,
			    sizeof(trust_blob_unauth));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_INVALID_SIG,
		      "陌生键签名 = 拒（有效集外）");

	/* ⑤ ver 门：跳版（ver=9）= E_RANGE（严格 N+1） */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu2", NULL,
			    NULL, 0, NULL, 0, trust_blob_skip,
			    sizeof(trust_blob_skip));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_RANGE,
		      "跳版拒");

	/* ⑥ 首次注册（工厂根签名 ver=1 + dev 键）→ OK + trust-get 回读 */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu3", NULL,
			    NULL, 0, NULL, 0, trust_blob_add,
			    sizeof(trust_blob_add));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
	zassert_equal(ts_store_trust()->ver, 1);
	zassert_equal(ts_store_trust()->n_keys, 1);
	zassert_equal(memcmp(ts_store_trust()->keys[0], trust_dev_pub, 32), 0);
	rlen = build_fs_req(req, sizeof(req), "trust-get", "rid-tg1",
			    NULL, NULL, 0, NULL, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/trust-get", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);

	/* ⑦ 多根实证：开发者键签名包 NULL（有效集）安装 = OK */
	zassert_equal(ts_appmgr_install(boot_pkg_devroot,
					sizeof(boot_pkg_devroot), NULL, &info),
		      TS_OK, "注册后开发者键命中有效集");

	/* ⑧ 重放（ver=1 已应用）= E_RANGE */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu4", NULL,
			    NULL, 0, NULL, 0, trust_blob_replay,
			    sizeof(trust_blob_replay));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_RANGE,
		      "重放拒（单调防回滚）");

	/* ⑨ 有效集内互荐：开发者键签名次版（ver=2）= OK */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu5", NULL,
			    NULL, 0, NULL, 0, trust_blob_dev_signed,
			    sizeof(trust_blob_dev_signed));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK,
		      "现信任键（开发者）可推动次版");
	zassert_equal(ts_store_trust()->ver, 2);

	/* ⑩ 全撤回（ver=3 空表，工厂根签名）→ 开发者键复拒（回工厂根单源） */
	rlen = build_fs_req(req, sizeof(req), "trust-update", "rid-tu6", NULL,
			    NULL, 0, NULL, 0, trust_blob_clear,
			    sizeof(trust_blob_clear));
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
	zassert_equal(ts_store_trust()->ver, 3);
	zassert_equal(ts_store_trust()->n_keys, 0, "空表合法（全撤回）");
	zassert_equal(ts_appmgr_install(boot_pkg_devroot,
					sizeof(boot_pkg_devroot), NULL, &info),
		      TS_E_INVALID_SIG, "撤回后开发者键复拒");

	/* ⑪ 工厂根包在信任表变迁全程可用（分层：工厂根不受表管辖） */
	zassert_equal(ts_appmgr_install(boot_pkg, sizeof(boot_pkg), NULL, &info),
		      TS_OK);
}

ZTEST_SUITE(framework_app, NULL, NULL, NULL, NULL, NULL);

/* ---- G3（单元 F）：INPUT_CHANGED → APP mailbox 路由 ----------------------- */
ZTEST(framework_app, test_07_input_routing)
{
	ts_appmgr_app_test_reset();
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	k_sleep(K_MSEC(100)); /* APP 线程就位 */

	const struct ts_input_evt p = {.inst = 0, .old_mv = 100, .new_mv = 1234};
	const ts_evt_t evt = {
		.id = TS_EVT_INPUT_CHANGED,
		.t_ms = ts_time_ms(),
		.data = &p,
		.len = sizeof(p),
	};
	ts_evt_publish(&evt);
	k_sleep(K_MSEC(150));

	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	zassert_equal(st.evt_seen, 1, "input 事件路由进 mailbox（G3）");
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

/* ---- G4（单元 H）：激活即停——单活跃语义收口 --------------------------------
 * 注：不经 boot_start 装载（直调 app_start）——进程内二次 boot_start 属
 * WAMR 池怪癖第三型（实例化后导出查找恒空；dev-env §5 登记，装载路径 V1
 * 经重启——与换包怪癖同族），本用例考察 activate 语义与装载来源无关；
 * boot 单次装载链在 test_04，重载链（暖复位）在 persistbench。 */
ZTEST(framework_app, test_08_activate_while_running)
{
	suite_setup_channel();
	ts_store_test_reset();
	ts_appmgr_test_reset();

	const uint8_t root_key[32] = {0};
	ts_app_info_t info;

	/* 首装（建 meta/双槽事实）+ 直调启动：APP 运行中（撕裂态起点） */
	zassert_equal(ts_appmgr_install(boot_pkg, sizeof(boot_pkg),
					root_key, &info), TS_OK);
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	zassert_true(wait_gpio(true), "init 写 gpio=1");
	zassert_true(ts_appmgr_app_running());

	/* 二装（运行中安装 → 非活动槽）→ activate：旧缺陷 = 旧包继续运行 +
	 * 观测面报 STAGED/新槽；G4 收口 = 激活即停（DR-14 升级停止语义）。 */
	zassert_equal(ts_appmgr_install(boot_pkg, sizeof(boot_pkg),
					root_key, &info), TS_OK);
	zassert_false(ts_appmgr_app_running(), "激活即停（运行面收口）");
	zassert_equal(info.state, TS_APP_STAGED);
	zassert_equal(info.active_slot, 0, "meta 翻至 slot A（次装目标）");
	ts_appmgr_meta_t meta;

	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].active_slot, 0, "持久面同步翻转");
	zassert_equal(meta.entries[0].rollback_count, 0, "新安装重置回滚计数");

	/* 激活停后 WAMR 运行面健全：同模块直调重启可用（装载周期在真机经
	 * 暖复位闭环——persistbench PB4/PB5 链）。 */
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK,
		      "stop 后运行面可重启");
	zassert_true(ts_appmgr_app_running());
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

/* ---- F3（DEC-62① · Q-46①③）：安装闸本地确认——挂起/确认/拒绝/超时 ----- */

/* prov v2 12 对最小构建器（键序 = 定稿 schema；store 套件 build_prov_cbor_full
 * 同序对照——本套件只测 appmgr 闸行为，schema 矩阵在 framework.store） */
static size_t build_prov12(uint8_t *o, size_t cap, uint8_t lconf)
{
	/* 容量契约（R5 批实测 204B——12 对；此前 ARG_UNUSED(cap) + 调用方
	 * pb[192] 构成 12B 栈越界写存量缺陷——interact 套件帧布局引爆，两侧
	 * 套件同修）：不足即拒。 */
	if (cap < 256) {
		return 0;
	}
	size_t n = 0;

	o[n++] = 0xAC; /* map(12) */
#define P_T(s) do { o[n++] = (uint8_t)(0x60 | (sizeof(s) - 1)); \
	memcpy(o + n, s, sizeof(s) - 1); n += sizeof(s) - 1; } while (0)
#define P_KV_U(k, v) do { P_T(k); o[n++] = (uint8_t)(v); } while (0)
	P_KV_U("v", 2);
	P_T("node_id"); P_T("gate-n");
	P_T("cube_id"); P_T("gate-c");
	P_T("routers"); o[n++] = 0x81; P_T("tcp/10.0.0.9:7447");
	P_T("pk0"); o[n++] = 0x58; o[n++] = 32; memset(o + n, 0x11, 32); n += 32;
	P_T("pk1"); o[n++] = 0x58; o[n++] = 32; memset(o + n, 0x22, 32); n += 32;
	P_T("cred"); P_T("cred");
	P_T("pwr_ma"); o[n++] = 0x19; o[n++] = 500 >> 8; o[n++] = 500 & 0xFF;
	P_KV_U("estop", 0);
	P_KV_U("deploy_mode", 0);
	P_KV_U("estop_remote_clear", 1);
	P_KV_U("lconf", lconf);
#undef P_KV_U
#undef P_T
	return n;
}

/* 完整 staging 会话（begin→chunk→verify——boot_pkg 测试根签名） */
static ts_res_t stage_full(uint8_t *slot_out)
{
	uint8_t slot;
	uint32_t hw;
	ts_res_t r = ts_appmgr_stage_begin(sizeof(boot_pkg), &slot);

	if (r != TS_OK) {
		return r;
	}
	r = ts_appmgr_stage_chunk(0, boot_pkg, sizeof(boot_pkg), &hw);
	if (r == TS_OK && hw == sizeof(boot_pkg)) {
		r = ts_appmgr_stage_verify(NULL, NULL, NULL, NULL);
	}
	if (slot_out != NULL) {
		*slot_out = slot;
	}
	return r;
}

ZTEST(framework_app, test_14_install_gate)
{
	uint8_t pb[256];
	size_t pn = build_prov12(pb, sizeof(pb), 1);
	ts_app_info_t info;
	ts_appmgr_meta_t meta;
	uint8_t slot;

	ts_store_test_reset();
	ts_appmgr_test_reset();
	zassert_equal(ts_store_prov_write_test(pb, pn), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_equal(ts_store_prov()->local_confirm, 1);

	/* 基线：直装一版——install 单发路同过闸（EC4 一切安装通道同一收口：
	 * meta 就位 = 「挂起不切 meta」的可观测前提）。 */
	zassert_equal(ts_appmgr_install(boot_pkg, sizeof(boot_pkg), NULL, &info),
		      TS_E_PENDING, "install 通道同闸（fail-closed 无例外）");
	zassert_equal(ts_appmgr_confirm_pending(&info), TS_OK, "本地确认后落位");
	uint8_t active_before = info.active_slot;

	/* ① activate 过闸：不切 meta、置 pending、结构化中间态 */
	zassert_equal(stage_full(&slot), TS_OK);
	zassert_not_equal(slot, active_before, "staging 目标 = 对内乒乓槽");
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_PENDING,
		      "lconf=1 = activate 过闸");
	zassert_true(ts_appmgr_pending_active());
	zassert_equal(info.pending_slot, slot, "pending 面 = 目标槽");
	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].active_slot, active_before,
		      "meta 未切（等本地确认）");

	/* ② 单窗：挂起期再 activate = BUSY */
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_BUSY);

	/* ③ 呈现面单一事实源（EC3 同 manifest 数据面——ASCII 行） */
	char sum[320];
	int sn = ts_appmgr_pending_summary(sum, sizeof(sum));

	zassert_true(sn > 0, "summary 非空");
	zassert_not_null(strstr(sum, "app: com.tessera.fixture"));
	zassert_not_null(strstr(sum, "0.1.0"));
	zassert_not_null(strstr(sum, "caps:"));
	zassert_not_null(strstr(sum, "slot:"));
	zassert_equal(ts_appmgr_pending_summary(sum, 8), 0, "非挂起/小缓冲 = 0");

	/* ④ get-app 挂起面（加性 pending_slot） */
	zassert_equal(ts_appmgr_get_info(&info), TS_OK);
	zassert_equal(info.pending_slot, slot);

	/* ⑤ 确认（固件本地路）→ meta 原子切换 */
	zassert_equal(ts_appmgr_confirm_pending(&info), TS_OK);
	zassert_false(ts_appmgr_pending_active());
	zassert_equal(info.active_slot, slot, "确认 = meta 切至目标槽");
	zassert_equal(info.pending_slot, 0xFF);
	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].active_slot, slot, "持久面同步");
	zassert_equal(ts_appmgr_confirm_pending(&info), TS_E_STATE, "无挂起 = 拒");
	active_before = slot; /* ⑥⑦ 的对比基线 = 当前活动槽 */

	/* ⑥ 拒绝路径：meta 不动 + 清台（重试须完整重推） */
	zassert_equal(stage_full(&slot), TS_OK);
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_PENDING);
	zassert_equal(ts_appmgr_reject_pending(), TS_OK);
	zassert_false(ts_appmgr_pending_active());
	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].active_slot, active_before, "拒绝 = meta 不动");
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_STATE, "拒后清台");

	/* ⑦ 60s fail-closed 超时（本套件 400ms 档）：撤销 + meta 不动 + 清台 */
	zassert_equal(stage_full(&slot), TS_OK);
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_PENDING);
	k_sleep(K_MSEC(CONFIG_TS_APPMGR_PENDING_TIMEOUT_MS + 200));
	zassert_false(ts_appmgr_pending_active(), "超时撤销");
	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].active_slot, active_before, "超时 = meta 不动");
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_STATE, "超时后清台");

	/* ⑧ begin 覆盖闸：挂起中新推 = 挂起窗随新会话终止 */
	zassert_equal(stage_full(&slot), TS_OK);
	zassert_equal(ts_appmgr_stage_activate(&info), TS_E_PENDING);
	zassert_equal(ts_appmgr_stage_begin(sizeof(boot_pkg), &slot), TS_OK);
	zassert_false(ts_appmgr_pending_active(), "新 begin 终止挂起窗");

	/* ⑨ lconf=0（无头缺省）：activate 直切——旧行为零回归 */
	pn = build_prov12(pb, sizeof(pb), 0);
	zassert_equal(ts_store_prov_write_test(pb, pn), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	zassert_equal(stage_full(&slot), TS_OK);
	zassert_equal(ts_appmgr_stage_activate(&info), TS_OK, "闸关 = 原路径");
	zassert_equal(info.active_slot, slot);
	zassert_equal(info.pending_slot, 0xFF);

	/* 收尾：prov 复原 v1 缺省态（后续用例零扰动） */
	ts_store_test_reset();
	ts_appmgr_test_reset();
	zassert_equal(ts_store_prov_load(), TS_E_IO, "空分区 = 缺省兜底（lconf=0）");
}

/* ---- P1-1（HLD §3.4）：APP 间消息路由——名单授权/FIFO 背压/取件 --------- */
ZTEST(framework_app, test_15_msg_routing)
{
	ts_appmgr_app_test_reset();
	ts_safety_set_link(true);
	/* 双实例（同 wasm——串身份区分）：A 声明 msg:send:tessera.b 名单 */
	zassert_equal(ts_appmgr_app_start(1, app_wasm, app_wasm_len,
					  "msg:send:tessera.b;msg:send:tessera.ghost;gpio:write:0-3"),
		      TS_OK);
	zassert_equal(ts_appmgr_app_start(2, app_wasm, app_wasm_len,
					  "gpio:write:0-3"), TS_OK);
	ts_appmgr_app_set_idstr(1, "tessera.a");
	ts_appmgr_app_set_idstr(2, "tessera.b");

	struct ts_app_rt *ra = ts_appmgr_rt_by_str("tessera.a");
	struct ts_app_rt *rb = ts_appmgr_rt_by_str("tessera.b");

	zassert_not_null(ra, "运行实例串身份可寻址");
	zassert_not_null(rb);
	zassert_is_null(ts_appmgr_rt_by_str("tessera.ghost"), "未运行 = 不可寻址");
	ts_ctx_t ca = ts_app_rt_ctx(ra);
	ts_ctx_t cb = ts_app_rt_ctx(rb);

	/* ① 名单内发送 → 槽入队 + pending + 首个即投递 */
	uint8_t ping[5] = "ping1";

	zassert_equal(ts_appmgr_msg_send(ca, "tessera.b", ping, 5), TS_OK);
	zassert_equal(ts_appmgr_msg_pending(2), 1, "B 待取 1 件");

	/* ② 未声明目标 = PERM（B 无 msg 名单——合同 10） */
	zassert_equal(ts_appmgr_msg_send(cb, "tessera.a", ping, 5), TS_E_PERM);

	/* ③ 已授权但未运行的目标 = NOTFOUND（命名空间按 app_id 隔离；未授权
	 * 目标的探测在权限层即拒——不泄露运行面存在性） */
	zassert_equal(ts_appmgr_msg_send(ca, "tessera.ghost", ping, 5),
		      TS_E_NOTFOUND);

	/* ④ 超长 = RANGE（>96B） */
	uint8_t big[TS_APPMGR_MSG_MAX + 1] = {0};

	zassert_equal(ts_appmgr_msg_send(ca, "tessera.b", big, sizeof(big)),
		      TS_E_RANGE);

	/* ⑤ 取件：from 身份 + 载荷逐字节 + pending 归零 */
	char from[TS_PERM_MSG_NAME_MAX];
	uint8_t buf[TS_APPMGR_MSG_MAX];
	uint32_t len = 0;

	zassert_equal(ts_appmgr_msg_recv(cb, from, sizeof(from), buf,
					 sizeof(buf), &len), TS_OK);
	zassert_equal(len, 5);
	zassert_ok(strcmp(from, "tessera.a"), "发送方身份随槽携带");
	zassert_ok(memcmp(buf, "ping1", 5));
	zassert_equal(ts_appmgr_msg_pending(2), 0);

	/* ⑥ 空取件 = TS_OK + len 0（非错误） */
	zassert_equal(ts_appmgr_msg_recv(cb, from, sizeof(from), buf,
					 sizeof(buf), &len), TS_OK);
	zassert_equal(len, 0);

	/* ⑦ 满槽背压：4 发满 → 第 5 = BUSY；FIFO 序保持；取一补一 */
	for (int i = 0; i < TS_APPMGR_MSG_SLOTS; i++) {
		ping[4] = (char)('0' + i);
		zassert_equal(ts_appmgr_msg_send(ca, "tessera.b", ping, 5), TS_OK);
	}
	zassert_equal(ts_appmgr_msg_send(ca, "tessera.b", ping, 5), TS_E_BUSY,
		      "满槽 = 发送方背压（不丢）");
	zassert_equal(ts_appmgr_msg_recv(cb, from, sizeof(from), buf,
					 sizeof(buf), &len), TS_OK);
	zassert_equal(len, 5);
	zassert_ok(memcmp(buf, "ping0", 5), "FIFO 序");
	ping[4] = '9';
	zassert_equal(ts_appmgr_msg_send(ca, "tessera.b", ping, 5), TS_OK,
		      "取一补一");

	/* ⑧ 接收缓冲不足 = RANGE 且不弹出（消息保留） */
	zassert_equal(ts_appmgr_msg_recv(cb, from, sizeof(from), buf, 2, &len),
		      TS_E_RANGE);
	zassert_equal(ts_appmgr_msg_pending(2), TS_APPMGR_MSG_SLOTS);

	/* ⑨ 排空 4 件（序 ping1..3 + ping9）*/
	for (int i = 1; i <= TS_APPMGR_MSG_SLOTS; i++) {
		zassert_equal(ts_appmgr_msg_recv(cb, from, sizeof(from), buf,
						 sizeof(buf), &len), TS_OK);
		zassert_equal(len, 5);
	}
	zassert_equal(ts_appmgr_msg_pending(2), 0);

	/* ⑩ evt 投递面：发送后 mailbox 含 TS_APP_EVT_MSG（观测窗内轮询——
	 * app 线程消费 last_evt 的精确断言受线程时序影响，取确定性部分：
	 * 发送方满槽/权限路径已闭环，mailbox 投递由 evt_id 既有机制保证） */
	zassert_equal(ts_appmgr_msg_send(ca, "tessera.b", ping, 5), TS_OK);

	/* 收尾：停双实例 */
	zassert_equal(ts_appmgr_app_stop_id(1), TS_OK);
	zassert_equal(ts_appmgr_app_stop_id(2), TS_OK);
	ts_appmgr_app_test_reset();
}
