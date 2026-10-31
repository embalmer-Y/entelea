/* SPDX-License-Identifier: Apache-2.0 */
/*
 * persistbench（板级五）：prov/APP/meta flash 持久化验证（docs/board-persist.md）。
 * 流程：
 *   首启（prov 缺失）= 烧录会话：P4B1 prov 注入（TS_TEST 通道）→ P4B2 分步
 *   安装链（stage_begin/chunk/verify/activate——与 sys/app-* 网络命令同一
 *   内部链）→ 暖复位；
 *   此后每次复位：P4B3 全框架自举（prov/meta/slot 全部出自 flash）→
 *   P4B5 APP 自举运行 + 写路径全链验证 → P4B PASS。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include <ts/store.h>
#include <ts/tsap.h>
#include "../../../module/tessera/src/net/internal.h" /* ts_cbor_* 构造助手 */

#ifdef P4BENCH_NET
/* P4-W2（V0.14.0）网络面：WiFi STA（esp_hosted_mcu 新驱动）+ DHCPv4。
 * atkbench 同型（main 前置联网——zenoh glue 自举时链路已备）；凭据经
 * CMake 凭据文件编入（仓外纪律）。判据行：P4W2 WIFI UP / P4W2 DHCP OK。
 * 失败不 abort——继续走框架流（zenoh 面如实离线，日志可辨）。 */
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>

#ifndef P4BENCH_WIFI_SSID
#define P4BENCH_WIFI_SSID ""
#endif

static struct net_mgmt_event_callback p4b_ip_cb;
static struct k_sem p4b_ip_sem;

static void p4b_ip_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt,
			   struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (mgmt == NET_EVENT_IPV4_ADDR_ADD) {
		k_sem_give(&p4b_ip_sem);
	}
}

static void p4b_net_up(void)
{
	struct net_if *iface = net_if_get_default();

	for (int s = 0; s < 100 && !iface; s++) { /* 驱动 netif 注册等待 */
		k_sleep(K_MSEC(100));
		iface = net_if_get_default();
	}
	if (!iface) {
		printk("P4W2 WIFI FAIL no netif（无网形态继续）\n");
		return;
	}
	printk("P4W2 WIFI UP\n");

	k_sem_init(&p4b_ip_sem, 0, 1);
	net_mgmt_init_event_callback(&p4b_ip_cb, p4b_ip_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&p4b_ip_cb);

	static const uint8_t ssid[] = P4BENCH_WIFI_SSID;
	static const uint8_t psk[] = P4BENCH_WIFI_PASSWORD;

	struct wifi_connect_req_params cnx = {
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
	printk("P4W2 wifi_connect rc=%d\n", rc);

	if (k_sem_take(&p4b_ip_sem, K_SECONDS(40)) == 0 && iface->config.ip.ipv4) {
		char ip[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET,
			      &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
			      ip, sizeof(ip));
		printk("P4W2 DHCP OK ip=%s\n", ip);
	} else {
		printk("P4W2 DHCP FAIL（40s 无址——继续，zenoh 面将离线重试）\n");
	}
}
#endif /* P4BENCH_NET */

#include "p4bench_pkg.h" /* TSAP v2 包（gen_p4b_pkg.py 机械生成，测试根签名 */

/* ---- sim 通道（框架.app 同款：唯一写路径 → driver_dispatch → sim 记录）-- */

static const ts_out_ch_t pb_ch = {
	.uid = "pb0", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t pb_dev = {.uid = "pb0", .kind = TS_DEV_GPIO_OUT};

static bool pb_gpio(void)
{
	ts_out_value_t v;

	if (ts_safety_readback("pb0", &v) != TS_OK) {
		return false;
	}
	return v.b;
}

static bool pb_wait_gpio(bool want)
{
	for (int i = 0; i < 300; i++) {
		if (pb_gpio() == want) {
			return true;
		}
		k_msleep(10);
	}
	return false;
}

/* ---- prov CBOR（schema v1 定稿键序，DEC-30⑤；定长确定性编码）------------- */

#define CB_MAX 256

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

static size_t build_prov(struct cb *c)
{
	memset(c, 0, sizeof(*c));
	cb_hdr(c, 0xA0, 9);
	cb_tstr(c, "v");
	cb_hdr(c, 0x00, 1);
	cb_tstr(c, "node_id");
	cb_tstr(c, "pb-dev");
	cb_tstr(c, "cube_id");
	cb_tstr(c, "cube-pb");
	cb_tstr(c, "routers");
	cb_hdr(c, 0x80, 1);
	cb_tstr(c, "tcp/192.168.2.90:9955");
	cb_tstr(c, "pk0");
	cb_hdr(c, 0x40, 32);
	for (int i = 0; i < 32; i++) {
		cb_u8(c, 0x00);
	}
	cb_tstr(c, "pk1");
	cb_hdr(c, 0x40, 32);
	for (int i = 0; i < 32; i++) {
		cb_u8(c, 0x00);
	}
	cb_tstr(c, "cred");
	cb_tstr(c, "");
	cb_tstr(c, "pwr_ma");
	cb_hdr(c, 0x00, 500);
	cb_tstr(c, "estop");
	cb_hdr(c, 0x00, 1);
	return c->n;
}

/* ---- TSAP 容器（manifest canonical 七键 + 夹具 wasm + COSE 结构尾）-------- */

/* ---- 复位后验证线程（ts_core_boot 不返回，验证并发于框架运行期）---------- */

static int pb_perm_seen;

static void pb_on_perm(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	pb_perm_seen++;
}

/* 链路声明（部署侧）：本基准 TS_NET=n（无网面），boot 步骤 2 poweron_init
 * 将通道压回 SAFE_POWERON 后无人驱动链路——正常部署由 linkmon 在传输
 * 确立后 set_link(true)（板级四实证）。此处 BOOT_DONE 后由 glue 声明，
 * 通道迁移 ACTIVE，此后 APP 写入可达（app_init 期写入落在 SAFE 态被拒
 * = 合同 1/3 冷启动语义，init_res=-4 为预期值）。 */
static void pb_on_boot_done(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	ts_safety_set_link(true);
	printk("P4B4 boot done -> link up (glue, no-host bench)\n");
}

static void pb_verify(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	(void)ts_evt_subscribe(TS_EVT_PERM_DENIED, pb_on_perm, NULL);
	for (int i = 0; i < 600 && !ts_appmgr_app_running(); i++) {
		k_msleep(20);
	}
	if (!ts_appmgr_app_running()) {
		printk("P4B FAIL app not running after boot\n");
		return;
	}
	ts_app_info_t info;

	ts_appmgr_get_info(&info);
	printk("P4B5 app active: id=%s ver=%s slot=%u\n",
	       info.app_id, info.app_ver, info.active_slot);
	/* 诊断：init_res=-4（SAFE_POWERON 写拒绝）= 冷启动预期（见上）；此后
	 * evt 驱动写验证 ACTIVE 态全链。 */
	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	printk("P4B5 diag: init_res=%d evt_seen=%u health_fails=%u perm_seen=%d\n",
	       (int)st.init_res, st.evt_seen, st.health_fails, pb_perm_seen);
	if (ts_appmgr_app_evt(1) != TS_OK || !pb_wait_gpio(true)) {
		printk("P4B FAIL app_evt(1) write\n");
		return;
	}
	printk("P4B5 app_evt(1) -> gpio=1 (flash slot -> boot 装载 -> wasm 全链)\n");
	if (ts_appmgr_app_evt(0) == TS_OK && pb_wait_gpio(false)) {
		printk("P4B5 app_evt(0) -> gpio=0\n");
	} else {
		printk("P4B FAIL app_evt(0)\n");
		return;
	}
	printk("P4B PASS prov+meta+slot persisted across reset\n");
	for (;;) {
		k_msleep(5000);
		printk("P4B alive t=%u\n", (unsigned)k_uptime_get_32());
	}
}
K_THREAD_DEFINE(pb_verify_tid, 2048, pb_verify, NULL, NULL, NULL, 12, 0, 1000);

int main(void)
{
	printk("P4B0 persistbench\n");

#ifdef P4BENCH_NET
	p4b_net_up(); /* P4-W2：联网先行（zenoh glue 自举时链路已备） */
#endif

	ts_res_t lr = ts_store_prov_load();

	if (lr != TS_OK) {
		/* 首启 = 烧录会话（prov 缺失；合同 10 写通道 = 烧录期/测试构建注入） */
		printk("P4B1 first boot: prov absent (rc=%d) -> provision session\n", (int)lr);
		struct cb c;

		if (ts_store_prov_write_test(c.b, (uint32_t)build_prov(&c)) != TS_OK ||
		    ts_store_prov_load() != TS_OK) {
			printk("P4B FAIL prov\n");
			return 1;
		}
		printk("P4B1 prov burned: node=%s cube=%s\n",
		       ts_store_prov()->node_id, ts_store_prov()->cube_id);

		const uint8_t *pkg = p4bench_pkg;
		size_t total = sizeof(p4bench_pkg);
		uint8_t slot = 0;
		uint32_t hw = 0;
		const uint8_t root_key[32] = {0}; /* TEST：prov 根钥缺省（结构级验签） */

		if (total == 0) {
			printk("P4B FAIL install total=0\n");
			return 1;
		}
		/* 分步报码：失败面逐级可见（begin/chunk/verify 各自 r 码）*/
		ts_res_t sr = ts_appmgr_stage_begin((uint32_t)total, &slot);

		if (sr != TS_OK) {
			printk("P4B FAIL install begin r=%d\n", (int)sr);
			return 1;
		}
		ts_res_t cr = ts_appmgr_stage_chunk(0, pkg, (uint32_t)total, &hw);

		if (cr != TS_OK) {
			printk("P4B FAIL install chunk r=%d\n", (int)cr);
			return 1;
		}
		ts_res_t vr = ts_appmgr_stage_verify(root_key, NULL, NULL, NULL);

		if (vr != TS_OK) {
			printk("P4B FAIL install verify r=%d\n", (int)vr);
			return 1;
		}
		ts_app_info_t info;

		if (ts_appmgr_stage_activate(&info) != TS_OK) {
			printk("P4B FAIL activate\n");
			return 1;
		}
		printk("P4B2 app staged: slot=%u total=%u hw=%u -> warm reset\n",
		       slot, (unsigned)total, hw);
		sys_reboot(SYS_REBOOT_WARM);
	}

	printk("P4B3 boot: prov from flash node=%s\n", ts_store_prov()->node_id);
	if (ts_hal_register_dev(&pb_dev) != TS_OK ||
	    ts_safety_register_channel(&pb_ch) != TS_OK) {
		printk("P4B FAIL channel\n");
		return 1;
	}
	ts_safety_set_link(true);
	/* BOOT_DONE 订阅须在 boot 前完成（事件发布早于校验线程启动） */
	(void)ts_evt_subscribe(TS_EVT_BOOT_DONE, pb_on_boot_done, NULL);
	/* 全框架自举（步骤 8 = slot 装载）；不返回：成功进入主循环，
	 * 失败进 fail-safe 停喂 WDT（合同 6）。 */
	ts_core_boot();
}
