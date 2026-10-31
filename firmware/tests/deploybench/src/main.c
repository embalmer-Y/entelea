/* SPDX-License-Identifier: Apache-2.0 */
/* deploybench（板级十）：Agent→真机完整部署 E2E 的板侧载体
 * （docs/board-deploy.md）。链路：WiFi STA（glue = netbench 板级七定稿：
 * 固定 2s 无抖动重试 + 断线 DHCP 重启 + media_down 下沉）→ prov flash 持久
 * （首启 TS_TEST 烧入，此后复位全出自 flash——板级五语义）→ ts_core_boot
 * （net_init → zenoh 命令面〔sys/app-* 分步安装链〕+ 步骤 8 slot 装载）。
 * 部署编排全部在 Agent 侧（client.py 复用 deploy 链：发现→租约→分块→
 * verify→activate→get-app）；板侧只做：激活检测 → 自动暖复位 → 复位后
 * APP 自 slot 装载运行的观测宣告。
 *
 * DB* console 行 = 板侧时间线；DEPLOY_* = client.py 侧判定。
 * 凭证经 cmake 变量注入（DEPLOYBENCH_WIFI_*，不入仓库）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h> /* 须在 net_if.h 之后（参数表内 struct net_if 可见性） */
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/net.h>
#include <ts/safety.h>
#include <ts/store.h>

#ifndef DEPLOYBENCH_WIFI_SSID
#define DEPLOYBENCH_WIFI_SSID ""
#endif
#ifndef DEPLOYBENCH_WIFI_PASSWORD
#define DEPLOYBENCH_WIFI_PASSWORD ""
#endif
#ifndef DEPLOYBENCH_ROUTER
#define DEPLOYBENCH_ROUTER "tcp/192.168.2.91:9955"
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
		printk("DBw associated t=%u\n", (unsigned)k_uptime_get_32());
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

/* {"v":1,"node_id":"dbn","cube_id":"dbc","routers":["tcp/192.168.2.101:9957"],
 *  "pk0":32×00,"pk1":32×00,"cred":"","pwr_ma":500,"estop":0}（定稿键序，
 *  prov.c 固定走查序）。**数组 = 脚本机械生成并按固件 schema 走查验证**
 *  （~/project/logs/gen_db_prov.py：netbench 数组改 id 字节）——手抄曾丢
 *  6 字节致固件解析 fail-closed（板级十过程留痕）。router 字面量生成期
 *  固化——PC 侧 IP 变更须重生成。pk 全零：V1 固件验签 = COSE 结构级
 *  （tag18+array4，pkg.c verify_cose_minimal 已知限制），与 Agent 侧
 *  tsap_verify（真 ed25519）分层——板级十报告留痕。 */
static const uint8_t prov_blob[] = {
	0xa9, 0x61, 0x76, 0x01, 0x67, 0x6e, 0x6f, 0x64, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x64, 0x62, 0x6e, 0x67, 0x63, 0x75, 0x62, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x64, 0x62, 0x63, 0x67, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x72, 0x73,
	0x81, 0x76, 0x74, 0x63, 0x70, 0x2f, 0x31, 0x39, 0x32, 0x2e, 0x31, 0x36,
	0x38, 0x2e, 0x32, 0x2e, 0x31, 0x30, 0x31, 0x3a, 0x39, 0x39, 0x35, 0x37,
	0x63, 0x70, 0x6b, 0x30, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x63, 0x70, 0x6b, 0x31, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x64, 0x63, 0x72, 0x65, 0x64, 0x60, 0x66, 0x70,
	0x77, 0x72, 0x5f, 0x6d, 0x61, 0x19, 0x01, 0xf4, 0x65, 0x65, 0x73, 0x74,
	0x6f, 0x70, 0x00,
};

/* APP 部署目标通道（gpio inst 0 = dbled；PWM inst 1 = dbpwm）
 * MD1.1 增 PWM 通道：D3 呼吸灯 demo 限幅判据（limits max = 5000Hz|700‰，
 * periphbench PP3 同款——超限请求 → 落 700‰ + 审计 res=TS_E_RANGE） */
static const ts_out_ch_t db_led = {
	.uid = "dbled", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t db_dev = {
	.uid = "dbled", .kind = TS_DEV_GPIO_OUT,
};
/* PWM 打包域（LLD-ts-hal §3）：高 16 = hz/100、低 16 = permille */
#define PWM_PACK(hz, pm) ((((hz) / 100U) << 16) | (pm))
static const ts_out_ch_t db_pwm = {
	.uid = "dbpwm", .kind = TS_CH_PWM,
	.poweron = {.u = PWM_PACK(1000, 0)},
	.linkloss = {.u = PWM_PACK(1000, 0)},
	.fault = {.u = PWM_PACK(1000, 0)},
	.limits = {.min = PWM_PACK(1000, 0), .max = PWM_PACK(1000, 700),
		   .slew_per_ms = 0},
};
static const ts_hal_dev_desc_t db_pwm_dev = {
	.uid = "dbpwm", .kind = TS_DEV_PWM,
};

/* ---- 部署观测线程：激活检测 → 暖复位 → APP 运行宣告 --------------------- */

static void deploy_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* 等 zenoh 会话（WiFi+DHCP+zenoh 全链；上电首启 ~8s 量级） */
	for (int i = 0; i < 600 && ts_net_state() != TS_NET_CONNECTED; i++) {
		k_msleep(100);
	}
	if (ts_net_state() != TS_NET_CONNECTED) {
		printk("DB FAIL no session t=%u\n", (unsigned)k_uptime_get_32());
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

		printk("DB6 app running app_id=%s slot=%u t=%u\n",
		       info.app_id, info.active_slot,
		       (unsigned)k_uptime_get_32());
		printk("DB PASS\n");
		for (int i = 0;; i++) {
			k_msleep(2000);
			ts_app_info_t cur;

			if (ts_appmgr_get_info(&cur) == TS_OK &&
			    cur.state == TS_APP_STAGED &&
			    cur.active_slot != loaded) {
				printk("DB4 activated slot=%u（2s 后暖复位装载）\n",
				       cur.active_slot);
				k_msleep(2000);
				printk("DB5 warm reboot\n");
				sys_reboot(SYS_REBOOT_WARM);
			}
			if (i % 5 == 0) {
				printk("DB alive t=%u net=%d\n",
				       (unsigned)k_uptime_get_32(),
				       (int)ts_net_state());
			}
		}
	}

	/* 首启路径：等 Agent 激活（stage 槽 ≠ 当前 meta 活动槽）→ 暖复位
	 * （激活语义：STAGED 待加载周期——运行需重启装载，persistbench 同型） */
	printk("DB3 deploy target ready slot=%u t=%u（等待 Agent 部署）\n",
	       info.active_slot, (unsigned)k_uptime_get_32());
	for (;;) {
		k_msleep(300);
		ts_app_info_t cur;

		if (ts_appmgr_get_info(&cur) != TS_OK) {
			continue;
		}
		if (cur.active_slot != info.active_slot &&
		    cur.state == TS_APP_STAGED) {
			printk("DB4 activated slot=%u（2s 后暖复位装载）\n",
			       cur.active_slot);
			k_msleep(2000);
			printk("DB5 warm reboot\n");
			sys_reboot(SYS_REBOOT_WARM);
		}
	}
}
K_THREAD_DEFINE(deploy_watch_tid, 4096, deploy_watch, NULL, NULL, NULL, 13, 0, 0);

int main(void)
{
	printk("DB0 deploybench start\n");

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

	static const uint8_t ssid[] = DEPLOYBENCH_WIFI_SSID;
	static const uint8_t psk[] = DEPLOYBENCH_WIFI_PASSWORD;

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

	printk("DB1 wifi_connect rc=%d\n", rc);
	if (k_sem_take(&ip_ready, K_SECONDS(40)) != 0) {
		printk("DB FAIL dhcp timeout\n");
		return 1;
	}

	/* prov：flash 后端首启烧入，此后复位直读（板级五语义）；netbench 是
	 * RAM 后端每次启动重烧——本载体换持久路径（部署目标不依赖测试注入） */
	ts_res_t lr = ts_store_prov_load();
	ts_res_t wr = (lr == TS_OK)
		? TS_OK
		: ts_store_prov_write_test(prov_blob, sizeof(prov_blob));

	printk("DB2 prov load=%d burn=%d（0=OK/-8=空）\n", (int)lr, (int)wr);
	if (wr != TS_OK) {
		printk("DB FAIL prov\n");
		return 1;
	}
	/* PWM 后端 init 须在通道注册前（板级九纪律：缺绑定 = -ENODEV 如实上报） */
	if (ts_drv_pwm_init() != 0) {
		printk("DB FAIL pwm init\n");
		return 1;
	}
	(void)ts_safety_register_channel(&db_led);
	(void)ts_hal_register_dev(&db_dev);
	(void)ts_safety_register_channel(&db_pwm);
	(void)ts_hal_register_dev(&db_pwm_dev);
	ts_core_boot(); /* noreturn：net_init → zenoh 命令面 + 步骤 8 slot 装载 */
	return 0;
}
