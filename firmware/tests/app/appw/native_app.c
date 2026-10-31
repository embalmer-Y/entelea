/* SPDX-License-Identifier: Apache-2.0 */
/* framework.app 夹具 APP（DEC-43 接线批）：导入 ts_api_v1 natives 的
 * 最小自由固件 wasm（无 WASI，DEC-25）。
 * 行为：app_init 记录 ctx 并向 gpio0 写 1；app_evt(v)——v 带 0x100 位为
 * 测试控制通道（置健康失败模拟），否则记值并向 gpio0 写 v&1；
 * health_ping 返回 g_health（0 = 健康）。 */
extern int ts_gpio_write(int ctx, int inst, int v);

static int g_ctx;
static volatile int g_health;
static volatile int g_last_evt = -1;

__attribute__((export_name("app_init")))
int app_init(int ctx)
{
	g_ctx = ctx;
	return ts_gpio_write(ctx, 0, 1);
}

__attribute__((export_name("app_evt")))
int app_evt(int v)
{
	if (v & 0x100u) {
		g_health = 1; /* 测试控制：置健康失败 */
		return 0;
	}
	g_last_evt = (int)v;
	return ts_gpio_write(g_ctx, 0, v & 1u);
}

__attribute__((export_name("app_tick")))
int app_tick(void)
{
	return 0;
}

__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return g_health;
}

__attribute__((export_name("get_last_evt")))
int get_last_evt(void)
{
	return g_last_evt;
}
