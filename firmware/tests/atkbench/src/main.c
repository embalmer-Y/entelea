/* SPDX-License-Identifier: Apache-2.0 */
/* atkbench（K1）：ATK DNESP32-S3B bring-up + 第二 cube 部署链载体
 * （docs/board-deploy.md）。链路：WiFi STA（glue = netbench 板级七定稿：
 * 固定 2s 无抖动重试 + 断线 DHCP 重启 + media_down 下沉）→ prov flash 持久
 * （首启 TS_TEST 烧入，此后复位全出自 flash——板级五语义）→ ts_core_boot
 * （net_init → zenoh 命令面〔sys/app-* 分步安装链〕+ 步骤 8 slot 装载）。
 * 部署编排全部在 Agent 侧（client.py 复用 deploy 链：发现→租约→分块→
 * verify→activate→get-app）；板侧只做：激活检测 → 自动暖复位 → 复位后
 * APP 自 slot 装载运行的观测宣告。
 *
 * AK* console 行 = 板侧时间线（deploybench 同型）；DEPLOY_* = client 侧判定。
 * 凭证经 cmake 变量注入（ATKBENCH_WIFI_*，不入仓库）。 */
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h> /* 须在 net_if.h 之后（参数表内 struct net_if 可见性） */
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#ifdef CONFIG_TS_PERIPH
#include <ts/periph.h> /* F4：peripherals 自报 */
#endif
#include <ts/net.h>
#include <ts/safety.h>
#include <ts/store.h>

#include "ui_lcd.h" /* F3-1：LCD 呈现面（LVGL） */

#ifndef ATKBENCH_WIFI_SSID
#define ATKBENCH_WIFI_SSID ""
#endif
#ifndef ATKBENCH_WIFI_PASSWORD
#define ATKBENCH_WIFI_PASSWORD ""
#endif
#ifndef ATKBENCH_ROUTER
#define ATKBENCH_ROUTER "tcp/192.168.2.91:9955"
#endif

/* ---- WiFi glue（netbench 板级七定稿模式，全文照搬语义） ------------------ */

static struct net_mgmt_event_callback ip_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct k_sem ip_ready;
static atomic_t assoc_ok_evt;
static atomic_t disconn_evt;
static struct k_work_delayable wifi_retry_work;
static struct k_work assoc_work;
static struct wifi_connect_req_params cnx;

static void wifi_try_connect(struct k_work *w)
{
	ARG_UNUSED(w);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, net_if_get_default(),
			  &cnx, sizeof(cnx));

	if (rc != 0) {
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
}

static void wifi_assoc_work(struct k_work *w)
{
	/* net_mgmt 一律工作项上下文（回调内重入自激——板级七实证） */
	ARG_UNUSED(w);
	struct net_if *iface = net_if_get_default();

	if (atomic_set(&disconn_evt, 0) == 1) {
		ts_net_session_media_down();
		net_dhcpv4_restart(iface);
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
	if (atomic_set(&assoc_ok_evt, 0) == 1) {
		struct wifi_ps_params ps = {.enabled = WIFI_PS_DISABLED};

		(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
		printk("AKw associated t=%u\n", (unsigned)k_uptime_get_32());
	}
}

static void ip_handler(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event == NET_EVENT_IPV4_ADDR_ADD && net_if_is_up(iface)) {
		k_sem_give(&ip_ready);
	}
}

static void wifi_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;

		if (st != NULL && st->status == 0) {
			atomic_set(&assoc_ok_evt, 1);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&disconn_evt, 1);
	}
	if (atomic_get(&assoc_ok_evt) == 1 || atomic_get(&disconn_evt) == 1) {
		k_work_submit(&assoc_work);
	}
}

/* {"v":1,"node_id":"akn","cube_id":"akc","routers":["tcp/192.168.2.91:9955"],
 *  "pk0":32×00,"pk1":32×00,"cred":"","pwr_ma":500,"estop":0}（定稿键序，
 *  prov.c 固定走查序）。**数组 = 脚本机械生成**（~/project/logs/gen_prov_image.py
 *  ——gen_db_prov.py 迁移失落后的重写；手抄禁令，教训 27）。pk 全零：V1
 *  固件验签 = COSE 结构级（K2 后 ES8311/信任链载体——真验签链已在，
 *  K3 信任表上线后实机多根）。 */
/* v2 12 对形态（F3 · DEC-62①）：v1 前缀 + deploy_mode=orchestrated +
 * estop_remote_clear=0（本地清除板）+ lconf=1（安装闸开——真机判据
 * 载体）。**数组 = 脚本机械生成**（~/project/logs/gen_prov_image.py；
 * 手抄禁令，教训 27）。pk 全零：TEST 有效集 = 测试根 ∪ 信任表（K3）。 */
/* 机械生成（gen_prov_image.py——重跑复现；禁手抄，教训 27） */
static const uint8_t prov_blob[] = {
	0xac, 0x61, 0x76, 0x02, 0x67, 0x6e, 0x6f, 0x64, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x61, 0x6b, 0x6e, 0x67, 0x63, 0x75, 0x62, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x61, 0x6b, 0x63, 0x67, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x72, 0x73,
	0x81, 0x75, 0x74, 0x63, 0x70, 0x2f, 0x31, 0x39, 0x32, 0x2e, 0x31, 0x36,
	0x38, 0x2e, 0x32, 0x2e, 0x39, 0x31, 0x3a, 0x39, 0x39, 0x35, 0x35, 0x63,
	0x70, 0x6b, 0x30, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x63, 0x70, 0x6b, 0x31, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x64, 0x63, 0x72, 0x65, 0x64, 0x60, 0x66, 0x70, 0x77,
	0x72, 0x5f, 0x6d, 0x61, 0x19, 0x01, 0xf4, 0x65, 0x65, 0x73, 0x74, 0x6f,
	0x70, 0x00, 0x6b, 0x64, 0x65, 0x70, 0x6c, 0x6f, 0x79, 0x5f, 0x6d, 0x6f,
	0x64, 0x65, 0x00, 0x72, 0x65, 0x73, 0x74, 0x6f, 0x70, 0x5f, 0x72, 0x65,
	0x6d, 0x6f, 0x74, 0x65, 0x5f, 0x63, 0x6c, 0x65, 0x61, 0x72, 0x00, 0x65,
	0x6c, 0x63, 0x6f, 0x6e, 0x66, 0x01,
};
/* 198B（走查序由 ts_store_prov_validate 校验） */

/* APP 部署目标通道（gpio inst 0 = dbled；PWM inst 1 = dbpwm）
 * MD1.1 增 PWM 通道：D3 呼吸灯 demo 限幅判据（limits max = 5000Hz|700‰，
 * periphbench PP3 同款——超限请求 → 落 700‰ + 审计 res=TS_E_RANGE） */
static const ts_out_ch_t ak_led = {
	.uid = "akled", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t ak_dev = {
	.uid = "akled", .kind = TS_DEV_GPIO_OUT,
};
/* PWM 打包域（LLD-ts-hal §3）：高 16 = hz/100、低 16 = permille */
#define PWM_PACK(hz, pm) ((((hz) / 100U) << 16) | (pm))
static const ts_out_ch_t ak_pwm = {
	.uid = "akpwm", .kind = TS_CH_PWM,
	.poweron = {.u = PWM_PACK(1000, 0)},
	.linkloss = {.u = PWM_PACK(1000, 0)},
	.fault = {.u = PWM_PACK(1000, 0)},
	.limits = {.min = PWM_PACK(1000, 0), .max = PWM_PACK(1000, 700),
		   .slew_per_ms = 0},
};
static const ts_hal_dev_desc_t ak_pwm_dev = {
	.uid = "akpwm", .kind = TS_DEV_PWM,
};

/* ---- 部署观测线程：激活检测 → 暖复位 → APP 运行宣告 --------------------- */

/* K2（Q-44/DEC-61）：ES8311 音频输入输出真机自检（AKa* console 行）。
 * 录音判据 = 连续 8 帧非全零且峰值超阈（ADC 链活）；放音判据 = SPK_EN
 * 开 + 正弦帧写链 OK（声学侧由人耳/示波器确认——console 判据 = 驱动链）。*/
static void audio_selftest(void)
{
	int rc = ts_drv_es8311_init();

	printk("AKa0 es8311 init rc=%d\n", rc);
	if (rc != 0) {
		printk("AK AUDIO FAIL（无芯片/初始化拒——i2c/i2s 链见上）\n");
#ifdef ATKBENCH_SKIP_PERIPH_FAIL
		printk("AK WARN audio 降级（SKIP_PERIPH——TLS E2E）\n");
#endif
		return;
	}
	/* 静音保底：功放关断（NL 低有效——常静音态）。放音自检改
	 * ATKBENCH_AUDIO_PLAY 门控（默认关——板卡常驻安静；判据重跑 =
	 * cmake -DATKBENCH_AUDIO_PLAY=1）。 */
	ts_drv_es8311_spk(false);
	/* 录音：8 帧（160ms×8）——峰值统计 */
	static int16_t smp[320];
	uint32_t peak_all = 0;

	for (int f = 0; f < 8; f++) {
		int n = ts_drv_es8311_capture((uint8_t *)smp, 640);

		if (n != 640) {
			printk("AKa1 capture f=%d rc=%d\n", f, n);
			printk("AK AUDIO FAIL（录音链）\n");
			return;
		}
		uint32_t peak = 0;

		for (int i = 0; i < 320; i++) {
			uint32_t a = (smp[i] < 0) ? (uint32_t)(-smp[i]) : (uint32_t)smp[i];

			if (a > peak) {
				peak = a;
			}
		}
		if (peak > peak_all) {
			peak_all = peak;
		}
	}
	printk("AKa1 capture 8 frames peak=%u（>32 = ADC 链活）\n",
	       (unsigned)peak_all);
	if (peak_all <= 32) {
		printk("AKa1 frame head:");
		for (int i = 0; i < 16; i++) {
			printk(" %02x", ((uint8_t *)smp)[i]);
		}
		printk("\n");
		ts_drv_es8311_diag();
		/* 静默不早退——继续放音判据（二分：codec 整体死 vs ADC 特定静默）*/
	}
	/* 放音：SPK_EN 开 + 4 帧正弦（~1kHz：16 采样/周期 @16k）+ 关
	 * （ATKBENCH_AUDIO_PLAY 门控——默认关） */
#ifndef ATKBENCH_AUDIO_PLAY
	printk("AKa2 playback skipped（默认静音——ATKBENCH_AUDIO_PLAY=1 启用）\n");
	printk("AK AUDIO PASS（record-only，peak=%u）\n", (unsigned)peak_all);
	return;
#endif
	(void)ts_drv_es8311_spk(true);
	for (int f = 0; f < 4; f++) {
		for (int i = 0; i < 320; i++) {
			static const int16_t sin16[16] = {
				0, 12539, 23170, 30273, 32767, 30273, 23170, 12539,
				0, -12539, -23170, -30273, -32767, -30273, -23170, -12539,
			};

			smp[i] = sin16[i % 16] / 4; /* 幅度 -18dB（安全听感档）*/
		}
		int n = ts_drv_es8311_write((const uint8_t *)smp, 640);

		if (n != 640) {
			printk("AKa2 write f=%d rc=%d\n", f, n);
			printk("AK AUDIO FAIL（放音链）\n");
			return;
		}
	}
	k_msleep(150); /* 尾帧播完 */
	(void)ts_drv_es8311_spk(false);
	printk("AKa2 playback 4 sine frames + SPK_EN toggle OK\n");
	if (peak_all <= 32) {
		printk("AK AUDIO FAIL（录音静默但放音链 OK——ADC 侧配置域）\n");
		return;
	}
	printk("AK AUDIO PASS\n");
}

static void deploy_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
#ifdef CONFIG_TS_DRV_ES8311
	audio_selftest(); /* K2：上电即测（音频链独立于 zenoh）*/
#endif
	/* 等 zenoh 会话（WiFi+DHCP+zenoh 全链；上电首启 ~8s 量级） */
	for (int i = 0; i < 600 && ts_net_state() != TS_NET_CONNECTED; i++) {
		k_msleep(100);
	}
	if (ts_net_state() != TS_NET_CONNECTED) {
		printk("AK FAIL no session t=%u\n", (unsigned)k_uptime_get_32());
		return;
	}

	ts_app_info_t info;

	if (ts_appmgr_get_info(&info) != TS_OK) {
		return;
	}
	if (info.app_id[0] != '\0' && info.state == TS_APP_ACTIVE) {
		/* 复位后路径：步骤 8 已自 slot 装载并运行（Agent 侧 get-app
		 * 对拍 state=ACTIVE + app_id）。MD1.1 修复：运行中持续检测
		 * 后续激活（连续部署场景——板级十单次部署设计在 MD1 暴露：
		 * alive 循环不再看激活 = 第 2 个起 demo 永远 STAGED）。 */
		uint8_t loaded = info.active_slot;

		printk("AK6 app running app_id=%s slot=%u t=%u\n",
		       info.app_id, info.active_slot,
		       (unsigned)k_uptime_get_32());
		printk("AK PASS\n");
		for (int i = 0;; i++) {
			k_msleep(2000);
			ts_app_info_t cur;

			if (ts_appmgr_get_info(&cur) == TS_OK &&
			    cur.state == TS_APP_STAGED &&
			    cur.active_slot != loaded) {
				printk("AK4 activated slot=%u（2s 后暖复位装载）\n",
				       cur.active_slot);
				k_msleep(2000);
				printk("AK5 warm reboot\n");
				sys_reboot(SYS_REBOOT_WARM);
			}
			if (i % 5 == 0) {
				printk("AK alive t=%u net=%d\n",
				       (unsigned)k_uptime_get_32(),
				       (int)ts_net_state());
			}
		}
	}

	/* 首启路径：等 Agent 激活（stage 槽 ≠ 当前 meta 活动槽）→ 暖复位
	 * （激活语义：STAGED 待加载周期——运行需重启装载，persistbench 同型） */
	printk("AK3 deploy target ready slot=%u t=%u（等待 Agent 部署）\n",
	       info.active_slot, (unsigned)k_uptime_get_32());
	for (int i = 0;; i++) {
		k_msleep(300);
		ts_app_info_t cur;

		if (ts_appmgr_get_info(&cur) != TS_OK) {
			continue;
		}
		/* 观测盲区修复（P1 收口期实证：本分支 27 分钟静默被误读为板死
		 * ——与 AK PASS 分支同款 10s 存活行） */
		if (i % 33 == 32) {
			printk("AK alive t=%u net=%d（等待部署）\n",
			       (unsigned)k_uptime_get_32(), (int)ts_net_state());
		}
		if (cur.active_slot != info.active_slot &&
		    cur.state == TS_APP_STAGED) {
			printk("AK4 activated slot=%u（2s 后暖复位装载）\n",
			       cur.active_slot);
			k_msleep(2000);
			printk("AK5 warm reboot\n");
			sys_reboot(SYS_REBOOT_WARM);
		}
	}
}
K_THREAD_DEFINE(deploy_watch_tid, 4096, deploy_watch, NULL, NULL, NULL, 13, 0, 0);

/* ---- F1/F2 设备形态批：本地交互源（BOOT = IO0 低有效）--------------------
 * F1（本地交互源）：短按（<2s）→ arb 席位切换 REMOTE↔LOCAL——CG3 机制
 *（激活压制远程写 + ARB_CHANGED 事件 + 切换零输出）的真机接线。
 * F2（本地 estop 清除）：长按（≥2s）→ 本地清除链演示——[bench 合成
 * force akpwm（estop 面演示——物理 estop 无板载按键，合成触发如实标注）]
 * → ts_safety_clear_fault 本地路；远程清除已被 prov v2 estop_remote_clear=0
 * 关断（CG6 ISO 13850 偏离撤销形态——远程 sys/estop-clear = need local-clear）。 */
static const struct gpio_dt_spec ak_boot = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback ak_boot_cb;
static struct k_work ak_key_work;
static atomic_t ak_key_down_ms;

/* ---- F3-2 安装闸呈现（DEC-62①③）：事件 → LCD + AKP 判据行 + 手势模态 ---- */
static void ak_gate_evt(const ts_evt_t *evt, void *user)
{
	ARG_UNUSED(user);
	const ts_pending_evt_t *pl = evt->data;

	switch (evt->id) {
	case TS_EVT_PENDING_INSTALL: {
		static char sum[320];
		int n = ts_appmgr_pending_summary(sum, sizeof(sum));

		printk("AKP1 pending install slot=%u entry=%u（短按=确认 长按≥2s=拒绝）\n",
		       pl->slot, pl->entry);
		if (n > 0) {
			/* summary \n 分隔 → 行数组（EC3：同一 manifest 数据面） */
			const char *lines[12];
			uint8_t nl = 0;
			char *w = sum;

			while (nl < 12) {
				lines[nl++] = w;
				char *brk = strchr(w, '\n');

				if (brk == NULL) {
					break;
				}
				*brk = '\0';
				w = brk + 1;
			}
			(void)ak_ui_screen("INSTALL PENDING", lines, nl);
		}
		break;
	}
	case TS_EVT_INSTALL_CONFIRMED: {
		printk("AKP2 install confirmed slot=%u（meta 已切换——暖复位装载）\n",
		       pl->slot);
		static const char *const ok[] = {
			"install confirmed", "meta switched", "warm reboot next",
		};

		(void)ak_ui_screen("INSTALL", ok, 3);
		break;
	}
	case TS_EVT_INSTALL_REJECTED: {
		printk("AKP3 install rejected slot=%u（槽保留可重推）\n", pl->slot);
		static const char *const rj[] = {
			"install rejected", "slot kept", "re-push to retry",
		};

		(void)ak_ui_screen("INSTALL", rj, 3);
		break;
	}
	case TS_EVT_INSTALL_TIMEOUT: {
		printk("AKP4 install timeout slot=%u（fail-closed 撤销）\n", pl->slot);
		static const char *const to[] = {
			"install timeout", "cancelled fail-closed", "re-push to retry",
		};

		(void)ak_ui_screen("INSTALL", to, 3);
		break;
	}
	default:
		break;
	}
}

static void ak_key_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	uint32_t down = (uint32_t)atomic_get(&ak_key_down_ms);
	uint32_t held = k_uptime_get_32() - down;

	if (held < 50) {
		return; /* 抖动滤除 */
	}
	/* F3 手势模态（Q-46③）：pending 期短按=确认 / 长按≥2s=拒绝——模态
	 * 独占（与 F1 arb 切换互斥：pending 期不切席位）。确认/拒绝仅此本地路
	 *（无 zenoh 命令面 = 远程不可确认）。 */
	if (ts_appmgr_pending_active()) {
		if (held >= 2000) {
			ts_res_t r = ts_appmgr_reject_pending();

			printk("AKPR reject rc=%d（BOOT 长按本地路）\n", (int)r);
		} else {
			ts_app_info_t info;
			ts_res_t r = ts_appmgr_confirm_pending(&info);

			printk("AKPC confirm rc=%d slot=%u（BOOT 短按本地路——meta 已切换）\n",
			       (int)r, info.active_slot);
		}
		return;
	}
	if (held >= 2000) {
		printk("AKF1 bench 合成 force akpwm（estop 面演示触发）\n");
		int fr = ts_safety_force_channel_fault("akpwm");
		printk("AKF2 force rc=%d（通道入 SAFE_FAULT）\n", fr);
		k_msleep(400);
		ts_res_t cr = ts_safety_clear_fault();
		printk("AKC 本地 estop 清除 rc=%d（BOOT 长按本地路）\n", (int)cr);
	} else {
		ts_arb_owner_t cur = ts_safety_arb_owner();
		ts_arb_owner_t next = (cur == TS_ARB_REMOTE) ? TS_ARB_LOCAL : TS_ARB_REMOTE;
		ts_res_t r = ts_safety_set_arb(next);

		printk("AKL arb 席位切换 →%s rc=%d（本地交互源）\n",
		       next == TS_ARB_LOCAL ? "LOCAL" : "REMOTE", (int)r);
	}
}

static void ak_boot_isr(const struct device *port, struct gpio_callback *cb,
			uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	int v = gpio_pin_get_raw(ak_boot.port, ak_boot.pin);

	if (v == 0) {
		atomic_set(&ak_key_down_ms, (atomic_val_t)k_uptime_get_32());
	} else {
		k_work_submit(&ak_key_work);
	}
}

static void ak_local_input_init(void)
{
	if (!gpio_is_ready_dt(&ak_boot)) {
		printk("AK FAIL boot key not ready\n");
		return;
	}
	gpio_pin_configure_dt(&ak_boot, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&ak_boot, GPIO_INT_EDGE_BOTH);
	gpio_init_callback(&ak_boot_cb, ak_boot_isr, BIT(ak_boot.pin));
	gpio_add_callback_dt(&ak_boot, &ak_boot_cb);
	k_work_init(&ak_key_work, ak_key_work_fn);
	printk("AK8 本地交互源就绪（BOOT 短按=席位 长按=estop 演示链）\n");
}


int main(void)
{
	printk("AK0 atkbench start\n");

	struct net_if *iface = net_if_get_default();

	k_sem_init(&ip_ready, 0, 1);
	k_work_init_delayable(&wifi_retry_work, wifi_try_connect);
	k_work_init(&assoc_work, wifi_assoc_work);
	net_mgmt_init_event_callback(&ip_cb, ip_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);
	net_mgmt_init_event_callback(&wifi_cb, wifi_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	static const uint8_t ssid[] = ATKBENCH_WIFI_SSID;
	static const uint8_t psk[] = ATKBENCH_WIFI_PASSWORD;

	cnx = (struct wifi_connect_req_params){
		.ssid = ssid,
		.ssid_length = sizeof(ssid) - 1,
		.psk = psk,
		.psk_length = sizeof(psk) - 1,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &cnx, sizeof(cnx));

	printk("AK1 wifi_connect rc=%d\n", rc);
	if (k_sem_take(&ip_ready, K_SECONDS(40)) != 0) {
		/* F4（K3 夜间登记）：本 AP 对本板 DHCP 间歇不响应（5 启 4 败，
		 * tcpdump 证实 DISCOVER 后无 OFFER）。bench 级回退 = 静态地址
		 * （租约惯常值 .67），产品面 DHCP 语义不变。 */
		net_dhcpv4_stop(iface);
		static struct in_addr addr, netmask, gw;

		if (net_addr_pton(AF_INET, "192.168.2.67", &addr) == 0 &&
		    net_addr_pton(AF_INET, "255.255.255.0", &netmask) == 0 &&
		    net_addr_pton(AF_INET, "192.168.2.1", &gw) == 0 &&
		    net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) != NULL &&
		    net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask)) {
			net_if_ipv4_set_gw(iface, &gw);
			printk("AK1s static fallback 192.168.2.67\n");
		} else {
			printk("AK FAIL dhcp timeout + static fallback error\n");
			return 1;
		}
	}

	/* prov：flash 后端首启烧入，此后复位直读（板级五语义）；netbench 是
	 * RAM 后端每次启动重烧——本载体换持久路径（部署目标不依赖测试注入） */
	ts_res_t lr = ts_store_prov_load();
	ts_res_t wr = (lr == TS_OK)
		? TS_OK
		: ts_store_prov_write_test(prov_blob, sizeof(prov_blob));

	printk("AK2 prov load=%d burn=%d（0=OK/-8=空）\n", (int)lr, (int)wr);
	if (wr != TS_OK) {
		printk("AK FAIL prov\n");
		return 1;
	}
	/* PWM 后端 init 须在通道注册前（板级九纪律：缺绑定 = -ENODEV 如实上报） */
	if (ts_drv_pwm_init() != 0) {
		printk("AK FAIL pwm init\n");
		return 1;
	}
	/* F4：输出通道/设备经 periph 责任链单源注册（直连双注册与链注册
	 * uid 撞表——链自含 safety 通道 + hal 寻址） */
#ifndef CONFIG_TS_PERIPH
	(void)ts_safety_register_channel(&ak_led);
	(void)ts_hal_register_dev(&ak_dev);
	(void)ts_safety_register_channel(&ak_pwm);
	(void)ts_hal_register_dev(&ak_pwm_dev);
#else
	ARG_UNUSED(0);
	/* F4：peripherals 自报注册（caps 块——板载外设清单） */
	static const ts_periph_desc_t ak_periphs[] = {
		{.uid = "akled", .kind = TS_PK_GPIO, .dt_spec = NULL,
		 .safe = {.uid = "akled", .kind = TS_CH_GPIO,
			  .poweron = {.b = false}, .linkloss = {.b = false},
			  .fault = {.b = false}}},
		{.uid = "akpwm", .kind = TS_PK_PWM, .dt_spec = NULL,
		 .safe = {.uid = "akpwm", .kind = TS_CH_PWM,
			  .poweron = {.u = PWM_PACK(1000, 0)},
			  .linkloss = {.u = PWM_PACK(1000, 0)},
			  .fault = {.u = PWM_PACK(1000, 0)},
			  .limits = {.min = PWM_PACK(1000, 0),
				     .max = PWM_PACK(1000, 700),
				     .slew_per_ms = 0}}},
#ifdef CONFIG_TS_DRV_ES8311
		{.uid = "akcodec", .kind = TS_PK_ADC, .dt_spec = NULL},
#endif
	};

	int prc = 0;

	for (size_t pi = 0; pi < sizeof(ak_periphs) / sizeof(ak_periphs[0]); pi++) {
		int r = ts_periph_register(&ak_periphs[pi]);
		if (r != TS_OK) {
			prc = r;
		}
	}
	printk("AK9 peripherals 注册 rc=%d（caps 自报面）\n", prc);
#endif
	/* K4：SD 挂载（dsdbench 同型）——ts-fs 面（fs natives + fs_paths 门）
	 * 与 CG5-B 资源推送（sys/fs-* 命令）的前提。资源根目录幂等创建。 */
	static FATFS ak_fat;
	static struct fs_mount_t ak_mp = {
		.type = FS_FATFS,
		.mnt_point = "/SD:",
		.fs_data = &ak_fat,
	};
	int mr = fs_mount(&ak_mp);

	if (mr != 0 && mr != -EALREADY) {
#ifdef ATKBENCH_SKIP_PERIPH_FAIL
		/* TLS-E2E 助推（cmake 旗标）：SD 物理失联降级为告警——外设判据
		 * 缺省仍 FAIL；本旗标仅解锁 net/TLS 主链判据（plan v1.104）。 */
		printk("AK WARN sd mount r=%d（SKIP_PERIPH——TLS E2E 降级）\n", mr);
#else
		printk("AK FAIL sd mount r=%d\n", mr);
		return 1;
#endif
	}
	fs_mkdir("/SD:/res");
	struct fs_dirent de;

	if (fs_stat("/SD:/res", &de) == 0) {
		printk("AK7 SD mounted%s（/SD:/res 就绪）\n",
		       mr == -EALREADY ? " (already)" : "");
	} else {
		printk("AK7 SD mounted 但 /SD:/res 创建失败\n");
	}
	ak_local_input_init(); /* F1/F2 设备形态批：本地交互源 */
	/* F3-1（DEC-62②）：LCD bring-up——呈现失败如实上报但不阻塞 bench
	 * 主链（无屏/背光链路故障 ≠ 部署链故障；判据 = AKU1 行 + 屏上开机画面） */
	int urc = ak_ui_init();

	printk("AKU1 LCD+LVGL init rc=%d（0=OK——屏应显示开机画面）\n", urc);
	/* F3-2 安装闸事件订阅（呈现 + 手势模态数据面；pub.c 外发 …/sys/event） */
	{
		static const ts_evt_id_t gids[] = {
			TS_EVT_PENDING_INSTALL, TS_EVT_INSTALL_CONFIRMED,
			TS_EVT_INSTALL_REJECTED, TS_EVT_INSTALL_TIMEOUT,
		};

		for (size_t gi = 0; gi < sizeof(gids) / sizeof(gids[0]); gi++) {
			(void)ts_evt_subscribe(gids[gi], ak_gate_evt, NULL);
		}
	}
	ts_core_boot(); /* noreturn：net_init → zenoh 命令面 + 步骤 8 slot 装载 */
	return 0;
}
