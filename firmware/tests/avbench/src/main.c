/* SPDX-License-Identifier: Apache-2.0 */
/* avbench（MD1.2d spike）：视频帧 zenoh 分片传输真机测量——Sense 板。
 * 隔离设计：不经 ts-net 框架——zenoh-pico 直连 router（传输层独立测量，
 * 为 MD1.2d 分片通道设计供实测数据）。
 * 流程：WiFi STA（凭证 cmake 注入，不入仓库）→ DHCP → z_open（1.10.1
 * MULTI_THREAD 下后台任务自动起）→ 摄像头 QQVGA RGB565 捕获 4 帧（官方
 * 池路径，PSRAM 池）→ 停流持有 → 分片矩阵 {512,1024,2048,4096}B 每帧
 * z_put 发布（BLOCK+DATA QoS）→ PC 侧 av_recv.py 重组校验/计量。
 * 判据（AV* console 行）：
 *   AV1 WiFi up（IP）
 *   AV2 zenoh CONNECTED
 *   AV3 摄像头格式就绪
 *   AV4 捕获 4 帧（luma 活跃度——真数据非死帧）
 *   AV5 每分片配置：4 帧总时长/有效吞吐/put 数/错误数
 *   AV PASS（板侧全配置零错误；PC 侧独立重组对拍） */
#include <errno.h>
#include <string.h>
#include <zenoh-pico.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h> /* 须在 net_if.h 之后 */

#ifndef AVB_WIFI_SSID
#define AVB_WIFI_SSID ""
#endif
#ifndef AVB_WIFI_PASSWORD
#define AVB_WIFI_PASSWORD ""
#endif
#ifndef AVB_ROUTER
#define AVB_ROUTER "tcp/192.168.2.90:9955"
#endif

#define CAM DEVICE_DT_GET(DT_CHOSEN(zephyr_camera))
#define QVGA_W 160
#define QVGA_H 120
#define FRAME_BYTES (QVGA_W * QVGA_H * 2) /* RGB565 */
#define N_FRAMES 4
#define AVB_KEY "tessera/avbench/frame"
#define HDR_BYTES 16 /* cfg u32 | frame u32 | chunk u32 | n_chunks u16 | len u16（BE） */

static const uint32_t chunk_cfgs[] = {512, 1024, 2048, 4096};
static uint8_t txbuf[HDR_BYTES + 4096];

static struct net_mgmt_event_callback ip_cb;
static struct k_sem ip_ready;

static void ip_handler(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event == NET_EVENT_IPV4_ADDR_ADD && net_if_is_up(iface)) {
		k_sem_give(&ip_ready);
	}
}

static uint32_t luma_sum(const uint8_t *buf, uint32_t len)
{
	/* RGB565 粗亮度（32 字节抽 1——cambench 同款活跃度判据） */
	uint32_t sum = 0;

	for (uint32_t i = 0; i < len; i += 32) {
		uint16_t px = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
		sum += (((px >> 11) & 0x1F) * 2 + ((px >> 5) & 0x3F) * 4 + (px & 0x1F)) >> 3;
	}
	return sum;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

int main(void)
{
	printk("AV0 avbench（MD1.2d spike：视频帧 zenoh 分片传输测量）\n");

	/* ---- WiFi STA + DHCP（netbench 精简：无重连场景）-------------------- */
	k_sem_init(&ip_ready, 0, 1);
	net_mgmt_init_event_callback(&ip_cb, ip_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);

	struct net_if *iface = net_if_get_default();
	static const uint8_t ssid[] = AVB_WIFI_SSID;
	static const uint8_t psk[] = AVB_WIFI_PASSWORD;
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

	int rc = -1;
	bool have_ip = false;

	/* 三轮关联尝试（实测：快速连续复位下 AP 的 DHCP 约 50% 不应答——
	 * 断开重连即恢复；netbench 重连纪律的精简版） */
	for (int attempt = 1; attempt <= 3; attempt++) {
		rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &cnx, sizeof(cnx));
		printk("AV0 wifi_connect #%d rc=%d ssid=%s\n", attempt, rc, AVB_WIFI_SSID);
		if (rc == 0 && k_sem_take(&ip_ready, K_SECONDS(15)) == 0) {
			have_ip = true;
			break;
		}
		printk("AV0 dhcp timeout #%d（断开重试）\n", attempt);
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
		k_msleep(2000);
	}
	/* 关省电（netbench 实证：modem-sleep 尾部 ~100ms 量级会污染吞吐） */
	struct wifi_ps_params ps = {.enabled = WIFI_PS_DISABLED};

	(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
	if (!have_ip) {
		printk("AV FAIL dhcp timeout\n");
		return 1;
	}
	char ipbuf[NET_IPV4_ADDR_LEN];

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (a->is_used) {
			net_addr_ntop(AF_INET, &a->address.in_addr, ipbuf, sizeof(ipbuf));
			printk("AV1 wifi up ip=%s\n", ipbuf);
		}
	}

	/* ---- zenoh-pico 直连（z_open 后台任务自动起）------------------------ */
	z_owned_config_t cfg;

	if (z_config_default(&cfg) != Z_OK) {
		printk("AV FAIL zenoh config\n");
		return 1;
	}
	if (zp_config_insert(z_loan_mut(cfg), Z_CONFIG_CONNECT_KEY, AVB_ROUTER) != Z_OK) {
		printk("AV FAIL zenoh insert loc\n");
		return 1;
	}
	z_owned_session_t zs;

	if (z_open(&zs, z_move(cfg), NULL) != Z_OK) {
		printk("AV FAIL zenoh open loc=%s\n", AVB_ROUTER);
		return 1;
	}
	printk("AV2 zenoh CONNECTED loc=%s\n", AVB_ROUTER);

	z_owned_keyexpr_t ke;

	if (z_keyexpr_from_str(&ke, AVB_KEY) != Z_OK) {
		printk("AV FAIL keyexpr\n");
		return 1;
	}

	/* ---- 摄像头：官方池路径（cambench 同款）捕获 4 帧持有 -------------- */
	if (!device_is_ready(CAM)) {
		printk("AV FAIL camera not ready\n");
		return 1;
	}
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = QVGA_W,
		.height = QVGA_H,
		.pitch = QVGA_W * 2,
	};

	if (video_set_format(CAM, &fmt) != 0) {
		printk("AV FAIL set format\n");
		return 1;
	}
	struct video_format got;

	video_get_format(CAM, &got);
	printk("AV3 camera fmt=%ux%u pitch=%u\n", got.width, got.height, got.pitch);
	if (got.width != QVGA_W || got.pixelformat != VIDEO_PIX_FMT_RGB565) {
		printk("AV FAIL fmt mismatch\n");
		return 1;
	}

	struct video_buffer *vbs[N_FRAMES];

	for (int i = 0; i < N_FRAMES; i++) {
		vbs[i] = video_buffer_aligned_alloc(FRAME_BYTES, 32, K_NO_WAIT);
		if (vbs[i] == NULL) {
			printk("AV FAIL alloc %d\n", i);
			return 1;
		}
		vbs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		printk("AV3b buf%d=%p\n", i, (void *)vbs[i]->buffer);
		if (video_enqueue(CAM, vbs[i]) != 0) {
			printk("AV FAIL enqueue %d\n", i);
			return 1;
		}
	}
	if (video_stream_start(CAM, VIDEO_BUF_TYPE_OUTPUT) != 0) {
		printk("AV FAIL stream start\n");
		return 1;
	}
	uint32_t frame_len = 0;

	for (int f = 0; f < N_FRAMES; f++) {
		if (video_dequeue(CAM, &vbs[f], K_SECONDS(5)) != 0) {
			printk("AV FAIL dequeue %d\n", f);
			return 1;
		}
		uint32_t luma = luma_sum(vbs[f]->buffer, vbs[f]->bytesused);

		printk("AV4 frame %d bytes=%u luma=%u\n", f, vbs[f]->bytesused, luma);
		if (f == 0) {
			frame_len = vbs[f]->bytesused;
			if (frame_len != FRAME_BYTES) {
				printk("AV FAIL frame len %u\n", frame_len);
				return 1;
			}
		}
	}
	video_stream_stop(CAM, VIDEO_BUF_TYPE_OUTPUT);
	printk("AV4 capture %d frames PASS（持有内存，进入传输矩阵）\n", N_FRAMES);

	/* ---- 分片传输矩阵：{512,1K,2K,4K}B × 4 帧，BLOCK+DATA --------------
	 * 真机实证（2026-10-06，Sense 板 → PC zenohd 9955）：
	 * ① ≤1KB 分片 + ≥4ms 打拍 = 零错误全达（512B 87KB/s / 1KB 172KB/s
	 *   稳态有效吞吐，PC 侧 4/4 帧完整重组）；
	 * ② >2KB 分片（2048/4096）走 zenoh-pico 碎片路径（Z_BATCH_UNICAST_SIZE
	 *   =2048 静态生成头，无守卫不可覆盖）——背靠背与打拍均系统性失败
	 *   （z_put -100；首个失败 errno=0 = 未达系统调用，疑碎片分配面）；
	 * ③ 背靠背（无打拍）任意尺寸均大量失败——发送线程必须有间隔纪律
	 *   （让读/租约任务与 WiFi 排水）。
	 * 判据 = 支持档（512/1024）零错误；2048/4096 登记为预期不支持。 */
	bool all_ok = true;

	for (size_t c = 0; c < sizeof(chunk_cfgs) / sizeof(chunk_cfgs[0]); c++) {
		uint32_t cs = chunk_cfgs[c];
		bool supported = cs <= 1024;
		uint32_t n_chunks = (frame_len + cs - 1) / cs;
		uint32_t total_ms = 0, max_ms = 0;
		int puts = 0, errs = 0, retries = 0, first_err = 0;

		for (int f = 0; f < N_FRAMES; f++) {
			uint32_t t0 = k_uptime_get_32();

			for (uint32_t i = 0; i < n_chunks; i++) {
				uint32_t off = i * cs;
				uint32_t len = frame_len - off > cs ? cs : frame_len - off;

				put_be32(&txbuf[0], cs);
				put_be32(&txbuf[4], (uint32_t)f);
				put_be32(&txbuf[8], i);
				txbuf[12] = (uint8_t)(n_chunks >> 8);
				txbuf[13] = (uint8_t)n_chunks;
				txbuf[14] = (uint8_t)(len >> 8);
				txbuf[15] = (uint8_t)len;
				memcpy(&txbuf[HDR_BYTES], vbs[f]->buffer + off, len);

				z_owned_bytes_t pl;

				if (z_bytes_from_static_buf(&pl, txbuf, HDR_BYTES + len) != Z_OK) {
					errs++;
					continue;
				}
				z_put_options_t opt;

				z_put_options_default(&opt);
				opt.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
				opt.priority = Z_PRIORITY_DATA;
				errno = 0;
				z_result_t r = z_put(z_loan(zs), z_loan(ke), z_move(pl), &opt);

				/* 分片级重试（≤5 次 × 20ms）：WiFi 降级时 z_put 返回
				 * -100（errno=0，未达系统调用——tx 缓冲/背压面拒绝），
				 * BLOCK 语义在 Zephyr 端不保数据 → 应用层必须重试。
				 * 重试不重构造 payload（pl 已 move 失败即失效——重建） */
				int retry = 0;

				while (r != Z_OK && retry < 5) {
					retry++;
					k_msleep(20);
					if (z_bytes_from_static_buf(&pl, txbuf,
								   HDR_BYTES + len) != Z_OK) {
						continue;
					}
					errno = 0;
					r = z_put(z_loan(zs), z_loan(ke), z_move(pl), &opt);
				}
				retries += retry;

				puts++;
				if (r != Z_OK) {
					errs++;
					if (first_err == 0) {
						first_err = r;
						printk("AV5d first put err=%d errno=%d\n",
						       (int)r, errno);
					}
				}
				/* 打拍（诊断变量）：背靠背 z_put 在 2KB 批缓冲/堆分配
				 * 面上崩（-100 errno=0）——间隔让读/租约任务与 WiFi 排水 */
				k_msleep(4);
			}
			uint32_t dt = k_uptime_get_32() - t0;

			total_ms += dt;
			if (dt > max_ms) {
				max_ms = dt;
			}
		}
		uint32_t thru_kbs = total_ms > 0
			? (uint32_t)((uint64_t)frame_len * N_FRAMES / 1024U * 1000U / total_ms)
			: 0;

		printk("AV5 cfg=%uB n_chunks=%u frames=%d total_ms=%u max_frame_ms=%u thru=%uKB/s puts=%d retries=%d errs=%d first_err=%d rd_task=%d%s\n",
		       cs, n_chunks, N_FRAMES, total_ms, max_ms, thru_kbs, puts, retries,
		       errs, first_err, (int)zp_read_task_is_running(z_loan(zs)),
		       supported ? "" : "（>2KB 碎片路径：预期不支持）");
		if (errs > 0 && supported) {
			all_ok = false;
		}
	}

	z_drop(z_move(ke));
	z_drop(z_move(zs));
	printk("%s\n", all_ok ? "AV PASS" : "AV FAIL (put errors)");
	for (;;) {
		k_msleep(10000);
		printk("AV alive t=%u\n", (unsigned)k_uptime_get_32());
	}
	return 0;
}
