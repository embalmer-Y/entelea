/* SPDX-License-Identifier: Apache-2.0 */
/* boardbench 夹具 APP（板级二，效率 DoD 六项）：导入 ts_api_v1 natives 的
 * 自由固件 wasm（无 WASI，DEC-25）。宿主侧用 k_cycle 计时，本夹具只做
 * 受控负载 + 完成标记（通道值翻转），不携带任何测量逻辑。
 * evt 编码：bit0..3 = 命令；bit4 = 标记输出值；bit5.. = 参数 N。 */
extern int ts_gpio_write(int ctx, int inst, int v);
extern long long ts_time_ms(int ctx);

#define OP_BUSY 1u /* 纯解释循环 N 迭代 */
#define OP_RT 2u /* ts_time_ms 往返 N 次 */
#define OP_WR 3u /* 写路径 N 次（perm→safety→驱动） */
#define OP_ECHO 4u /* 立即回写（mailbox 时延探针） */
#define OP_WRIO 5u /* 真机 IO 写路径 N 次（inst 3 → 真寄存器） */

#define CH_ECHO 0
#define CH_MARK 1
#define CH_DATA 2
#define CH_REAL 3

static int g_ctx;
static volatile unsigned g_sink; /* 防优化 Sink */

__attribute__((export_name("app_init")))
int app_init(int ctx)
{
	g_ctx = ctx;
	return ts_gpio_write(ctx, CH_MARK, 1);
}

__attribute__((export_name("app_evt")))
int app_evt(int v)
{
	unsigned op = (unsigned)v & 0xFu;
	unsigned out = ((unsigned)v >> 4) & 1u;
	unsigned n = (unsigned)v >> 5;

	switch (op) {
	case OP_BUSY: {
		/* volatile 每迭代 RMW：防 LLVM 闭合式折叠（等差求和 O(1) 化）——
		 * 板级二实证：折叠后 300k 迭代 119µs 假数据 */
		volatile unsigned *sinkp = &g_sink;

		for (unsigned i = 0; i < n; i++) {
			*sinkp += i;
		}
		return ts_gpio_write(g_ctx, CH_MARK, (int)out);
	}
	case OP_RT: {
		long long t = 0;
		for (unsigned i = 0; i < n; i++) {
			t += ts_time_ms(g_ctx);
		}
		g_sink = (unsigned)t;
		return ts_gpio_write(g_ctx, CH_MARK, (int)out);
	}
	case OP_WR:
		for (unsigned i = 0; i < n; i++) {
			ts_gpio_write(g_ctx, CH_DATA, (int)(i & 1u));
		}
		return ts_gpio_write(g_ctx, CH_MARK, (int)out);
	case OP_WRIO:
		for (unsigned i = 0; i < n; i++) {
			ts_gpio_write(g_ctx, CH_REAL, (int)(i & 1u));
		}
		return ts_gpio_write(g_ctx, CH_MARK, (int)out);
	case OP_ECHO:
		return ts_gpio_write(g_ctx, CH_ECHO, (int)out);
	}
	g_sink = op;
	return -1;
}

__attribute__((export_name("app_tick")))
void app_tick(void)
{
}

__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return 0;
}
