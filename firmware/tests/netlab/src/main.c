/* SPDX-License-Identifier: Apache-2.0 */
/*
 * L3 联调镜像（LLD-ts-net §8-L3）：真实 zenoh 会话对 host 侧 zenohd router。
 * 场景：TAP zeth（host=192.0.2.2，固件=192.0.2.1）→ 发现/命令回执/心跳保持
 * 与断链判定。非 twister 用例——由 PC 侧 Python 客户端（eclipse-zenoh）驱动
 * 断言，结果留痕 docs（M3a.2 L3 记录）。
 * prov = CONFIG_TS_TEST 注入通道烧入（生产 = 外部工具 / MA3 deploy_push_prov）；
 * prov CBOR 为**固件定稿键序**手工构造（cbor2 canonical 会按键排序——与固件
 * 确定性子集解码器的固定键序不一致，此处字节序即规格序）。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/net.h>
#include <ts/safety.h>
#include <ts/store.h>

static void *probe_thr(void *a)
{
	ARG_UNUSED(a);
	return NULL;
}

/* 裸 socket/POSIX 探针（L3 排障）：隔离 Zephyr 层与 zenoh-pico 层。 */
static void socket_probe(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	printk("[probe] socket fd=%d errno=%d\n", fd, errno);
	if (fd < 0) {
		return;
	}
	struct sockaddr_in addr = {0};

	addr.sin_family = AF_INET;
	addr.sin_port = htons(7447);
	inet_pton(AF_INET, "192.0.2.2", &addr.sin_addr);
	int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));

	printk("[probe] connect rc=%d errno=%d\n", rc, errno);
	if (rc == 0) {
		char msg[] = "tessera-probe";
		int n = send(fd, msg, sizeof(msg), 0);

		printk("[probe] send n=%d errno=%d\n", n, errno);
	}
	close(fd);
	printk("[probe] closed\n");

	/* zenoh-pico _z_condvar_init 同款序列：condattr + MONOTONIC + cond_init */
	pthread_condattr_t ca;
	int rc1 = pthread_condattr_init(&ca);
	int rc2 = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
	int rc3 = pthread_cond_init(&cv, &ca);

	printk("[probe] condattr_init=%d setclock=%d cond_init=%d\n", rc1, rc2, rc3);

	/* zenoh-pico setsockopt 疑点：SO_RCVTIMEO / SO_SNDTIMEO / TCP_NODELAY */
	int fd2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct timeval tv = {.tv_sec = 1};
	int one = 1;
	int rs = setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	int ws = setsockopt(fd2, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	int ns = setsockopt(fd2, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	printk("[probe] so_rcvtimeo=%d so_sndtimeo=%d tcp_nodelay=%d\n", rs, ws, ns);
	close(fd2);

	/* pthread 创建矩阵：NULL attr / setstacksize(5120) / setstacksize(10240) */
	pthread_t t1, t2, t3;
	int r1 = pthread_create(&t1, NULL, probe_thr, NULL);
	pthread_attr_t at;
	int ra = pthread_attr_init(&at);
	int rs1 = pthread_attr_setstacksize(&at, 5120);
	int r2 = pthread_create(&t2, &at, probe_thr, NULL);
	int rs2 = pthread_attr_setstacksize(&at, 10240);
	int r3 = pthread_create(&t3, &at, probe_thr, NULL);

	printk("[probe] create(null)=%d attr_init=%d ssz5120=%d create5120=%d ssz10240=%d create10240=%d\n",
	       r1, ra, rs1, r2, rs2, r3);
}

static const ts_out_ch_t l3_led = {
	.uid = "l3led", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t l3_dev = {
	.uid = "l3led", .kind = TS_DEV_GPIO_OUT,
};

/* {"v":1,"node_id":"l3n","cube_id":"l3c","routers":["tcp/192.0.2.2:7447"],
 *  "pk0":32×00,"pk1":32×00,"cred":"","pwr_ma":500,"estop":0}（定稿键序） */
static const uint8_t prov_blob[155] = {
	0xA9, 0x61, 0x76, 0x01, 0x67, 0x6E, 0x6F, 0x64, 0x65, 0x5F, 0x69, 0x64,
	0x63, 0x6C, 0x33, 0x6E, 0x67, 0x63, 0x75, 0x62, 0x65, 0x5F, 0x69, 0x64,
	0x63, 0x6C, 0x33, 0x63, 0x67, 0x72, 0x6F, 0x75, 0x74, 0x65, 0x72, 0x73,
	0x81, 0x72, 0x74, 0x63, 0x70, 0x2F, 0x31, 0x39, 0x32, 0x2E, 0x30, 0x2E,
	0x32, 0x2E, 0x32, 0x3A, 0x37, 0x34, 0x34, 0x37, 0x63, 0x70, 0x6B, 0x30,
	0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x70,
	0x6B, 0x31, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x64, 0x63, 0x72, 0x65, 0x64, 0x60, 0x66, 0x70, 0x77, 0x72, 0x5F, 0x6D,
	0x61, 0x19, 0x01, 0xF4, 0x65, 0x65, 0x73, 0x74, 0x6F, 0x70, 0x00,
};

int main(void)
{
	k_sleep(K_SECONDS(2)); /* 等 TAP 载波/ARP 就绪 */
	socket_probe();
	/* 首启空白 → 经测试通道烧入（RAM 后端同进程持久；重启镜像重新烧入） */
	ts_res_t lr = ts_store_prov_load();
	ts_res_t wr = (lr == TS_OK) ? TS_OK : ts_store_prov_write_test(prov_blob, sizeof(prov_blob));

	printk("[l3] prov first_load=%d burn=%d\n", (int)lr, (int)wr);
	(void)ts_safety_register_channel(&l3_led);
	(void)ts_hal_register_dev(&l3_dev);
	ts_core_boot(); /* noreturn；表尾含 net_init → zenoh 传输 + 周期驱动 */
	return 0;
}
