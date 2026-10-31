/* framework.threads 夹具（M19a，DEC-55/Q-37 A）：宿主侧 spawn 编排
 * 消费者——共享线性内存（-Wl,--shared-memory）+ 原子内建跨线程累加。
 * 状态机（app_tick 推进；gpio 0 = PASS 旗 / gpio 1 = FAIL 旗——sim 断言
 * 经安全层影子通道，tests/app 同款纪律）：
 *   state 0：spawn worker(1000) → h1（<0 = FAIL 旗——未申报 threads 用例）；
 *            spawn worker(2000) → h2（<0 = 并发 cap 拒绝，标记重试）；
 *   state 1：join(h1)（阻塞有界——L2 计量封顶 worker）；
 *   state 2：cap 拒绝形态 → 重 spawn h2；否则 join(h2)；
 *   state 3：校验 g_counter == 3000 → PASS 旗，否则 FAIL 旗。
 * worker = 一次性任务（原子累加 arg 次，返回 arg——join 回传）。 */
extern int ts_gpio_write(int ctx, int inst, int v);
extern int ts_log_write(int ctx, int lvl, const char *msg, int len);
extern int ts_thread_start(int ctx, const char *name, int name_len, int arg);
extern int ts_thread_join(int ctx, int handle);

static int g_counter; /* 共享线性内存（兄弟实例共享） */
static int g_state;
static int g_h1, g_h2;
static int g_r1, g_r2;
static int g_cap_retry;

int health_ping(void)
{
	return 0;
}

int app_init(int app_id)
{
	(void)app_id;
	g_counter = 0;
	g_state = 0;
	g_h1 = g_h2 = 0;
	g_r1 = g_r2 = 0;
	g_cap_retry = 0;
	return 0;
}

int worker(int arg)
{
	for (int i = 0; i < arg; i++) {
		__atomic_fetch_add(&g_counter, 1, __ATOMIC_RELAXED);
	}
	return arg;
}

static void log_line(const char *s)
{
	int n = 0;

	while (s[n] != 0) {
		n++;
	}
	ts_log_write(0, 3, s, n);
}

int app_tick(void)
{
	switch (g_state) {
	case 0:
		g_h1 = ts_thread_start(0, "worker", 6, 1000);
		if (g_h1 < 0) {
			log_line("thrd spawn1 rejected\n");
			ts_gpio_write(0, 1, 1); /* FAIL 旗（未申报/未启用） */
			g_state = 4;
			break;
		}
		g_h2 = ts_thread_start(0, "worker", 6, 2000);
		if (g_h2 < 0) {
			g_cap_retry = 1; /* 并发 cap 拒绝（threads=2 用例预期） */
		}
		g_state = 1;
		break;
	case 1:
		g_r1 = ts_thread_join(0, g_h1);
		g_state = 2;
		break;
	case 2:
		if (g_cap_retry) {
			g_h2 = ts_thread_start(0, "worker", 6, 2000);
			if (g_h2 < 0) {
				ts_gpio_write(0, 1, 1); /* 重试仍拒 = 非预期 */
				g_state = 4;
				break;
			}
			g_cap_retry = 0;
			break; /* 下一 tick join */
		}
		g_r2 = ts_thread_join(0, g_h2);
		g_state = 3;
		break;
	case 3:
		ts_gpio_write(0, 0,
			      (g_counter == 3000 && g_r1 == 1000 && g_r2 == 2000)
				? 1
				: 0);
		g_state = 4; /* 终态（PASS/FAIL 旗已落） */
		break;
	default:
		break;
	}
	return 0;
}
