/* SPDX-License-Identifier: Apache-2.0 */
/* ts-hal L1/L2 测试（LLD-ts-hal §7）：文法解析全分支 / 越权裁决 / API 面 /
 * 实例注册边界。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>

/* ---- 文法解析全分支（L1）------------------------------------------------ */

ZTEST(framework_hal, test_03_perm_parse_valid)
{
	ts_perm_table_t t;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("gpio:write:0-3", &t), TS_OK);
	zassert_equal(ts_perm_parse("adc:read:0,2", &t), TS_OK);
	zassert_equal(ts_perm_parse("power:set:1", &t), TS_OK);
	zassert_equal(ts_perm_parse("sys:read:0", &t), TS_OK);
	zassert_equal(ts_perm_parse("gpio:read:5", &t), TS_OK);
	zassert_equal(ts_perm_parse("pwm:set:0-15", &t), TS_OK);

	/* 验证位图 */
	uint32_t gpio_write = t.bitmap[TS_PERM_CLASS_GPIO][TS_PERM_OP_WRITE];

	zassert_true(gpio_write & 0x0F, "gpio:write:0-3 → bits 0-3");
	zassert_false(gpio_write & 0x10, "bit 4 not set");
	uint32_t adc_read = t.bitmap[TS_PERM_CLASS_ADC][TS_PERM_OP_READ];

	zassert_true(adc_read & 0x05, "adc:read:0,2 → bits 0,2");
	zassert_false(adc_read & 0x02, "bit 1 not set");
}

ZTEST(framework_hal, test_04_perm_parse_invalid)
{
	ts_perm_table_t t;

	ts_perm_table_init(&t);
	/* 非法类/操作/实例格式 */
	zassert_equal(ts_perm_parse(NULL, &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("bad:write:0", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("gpio:bad:0", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("gpio:write", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("gpio:write:xyz", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("gpio:write:0-", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("gpio:write:3-1", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("gpio:write:99", &t), TS_E_PARAM); /* >31 */
}

/* P1-1（HLD §3.4）：msg 类名单形态——文法矩阵 + 名单裁决 */
ZTEST(framework_hal, test_16_perm_msg_list_form)
{
	ts_perm_table_t t;

	ts_perm_table_init(&t);
	/* 合法名单（重复幂等；串身份文法 = zenoh key 段同源） */
	zassert_equal(ts_perm_parse("msg:send:tessera.demo.b", &t), TS_OK);
	zassert_equal(ts_perm_parse("msg:send:tessera.demo.b", &t), TS_OK,
		      "重复声明幂等");
	zassert_equal(ts_perm_parse("msg:send:a-2.x_z", &t), TS_OK);
	/* 纯数字串 = 合法名（app_id 文法 [a-z0-9.-] 允许——无数值实例歧义面：
	 * 名单语义统一为串匹配） */
	zassert_equal(ts_perm_parse("msg:send:0", &t), TS_OK);
	zassert_equal(ts_perm_parse("msg:read:tessera.b", &t), TS_E_PARAM,
		      "msg 非 send op = 拒（recv 被动无权限门——无消费者形态）");
	/* 非法名字（越界字符/超长/空） */
	zassert_equal(ts_perm_parse("msg:send:tessera demo", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("msg:send:bad/name", &t), TS_E_PARAM);
	zassert_equal(ts_perm_parse("msg:send:", &t), TS_E_PARAM);
	char cap[64] = "msg:send:";

	memset(cap + 9, 'a', 50);
	cap[59] = '\0';
	zassert_equal(ts_perm_parse(cap, &t), TS_E_PARAM, "超长名字");
	/* 名单上限 4：已 3（b/a-2.x_z + read 面）→ 补第 4 OK、第 5 = NOMEM */
	zassert_equal(ts_perm_parse("msg:send:t4", &t), TS_OK);
	zassert_equal(ts_perm_parse("msg:send:t5", &t), TS_E_NOMEM, "名单上限 4");

	/* 名单裁决（绑定 ctx 后 check_msg） */
	ts_ctx_t ctx;

	zassert_equal(ts_hal_bind_context(&ctx, 0x21, &t), TS_OK);
	zassert_equal(ts_perm_check_msg(ctx, "tessera.demo.b"), TS_OK,
		      "声明目标命中");
	zassert_equal(ts_perm_check_msg(ctx, "tessera.demo.ghost"), TS_E_PERM,
		      "未声明目标 = 拒（合同 10 留痕在 perm 层）");
	ts_ctx_t unbound = {.app_id = 0x7F};

	zassert_equal(ts_perm_check_msg(unbound, "tessera.demo.b"), TS_E_PERM,
		      "未绑定上下文 = 拒");
	zassert_equal(ts_perm_check_msg(ctx, NULL), TS_E_PARAM);
	ts_hal_unbind_context(&ctx);
}

/* ---- 越权裁决（L1，含边界实例号）---------------------------------------- */

ZTEST(framework_hal, test_02_perm_check_allowed_and_denied)
{
	ts_perm_table_t t;
	ts_ctx_t ctx;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("gpio:write:0-3", &t), TS_OK);
	zassert_equal(ts_perm_parse("gpio:read:5", &t), TS_OK);

	zassert_equal(ts_hal_bind_context(&ctx, 1, &t), TS_OK);

	/* 授权范围 */
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, 0), TS_OK);
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, 3), TS_OK);
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_READ, 5), TS_OK);

	/* 越权 */
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, 4), TS_E_PERM);
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_READ, 0), TS_E_PERM);
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_ADC, TS_PERM_OP_READ, 0), TS_E_PERM);
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_PWM, TS_PERM_OP_SET, 0), TS_E_PERM);

	/* 边界实例号 */
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, 31), TS_E_PERM);

	/* 未绑定 ctx */
	ts_ctx_t rogue = {.app_id = 99};
	zassert_equal(ts_perm_check(rogue, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, 0), TS_E_PERM);

	ts_hal_unbind_context(&ctx);
	zassert_equal(ts_perm_check(ctx, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, 0), TS_E_PERM);
}

/* ---- ts_api_v1（L2：写 → 安全层传播）----------------------------------- */

ZTEST(framework_hal, test_01_gpio_write_via_safety)
{
	/* 注册输出通道（ts-safety）+ HAL 设备实例 */
	static const ts_out_ch_t led_ch = {
		.uid = "led1", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
	};
	static const ts_hal_dev_desc_t led_dev = {
		.uid = "led1", .kind = TS_DEV_GPIO_OUT,
	};
	zassert_equal(ts_safety_register_channel(&led_ch), TS_OK);
	zassert_equal(ts_hal_register_dev(&led_dev), TS_OK);

	/* 绑定 ctx with gpio:write:0 */
	ts_perm_table_t t;
	ts_ctx_t ctx;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("gpio:write:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 2, &t), TS_OK);

	/* 链路未建立 → commit 被拒（安全层拦截传播） */
	zassert_equal(ts_gpio_write(ctx, 0, true), TS_E_STATE);

	/* 链路建立 → 写成功 */
	ts_safety_set_link(true);
	zassert_equal(ts_gpio_write(ctx, 0, true), TS_OK);
	ts_out_value_t rb;

	zassert_equal(ts_safety_readback("led1", &rb), TS_OK);
	zassert_true(rb.b, "gpio write propagated through safety");

	/* 越权实例 → TS_E_PERM */
	zassert_equal(ts_gpio_write(ctx, 1, true), TS_E_PERM);
	zassert_equal(ts_gpio_write(ctx, 0, true), TS_OK); /* 0 仍可用 */

	ts_hal_unbind_context(&ctx);
}

/* ---- pwm 参数域（IR-08：超打包域拒绝，防静默截断）------------------------ */

ZTEST(framework_hal, test_05_pwm_param_range)
{
	static const ts_out_ch_t pwm_ch = {
		.uid = "pwm1", .kind = TS_CH_PWM,
		.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 0xFFFFFFFFU, .slew_per_ms = 0},
	};
	static const ts_hal_dev_desc_t pwm_dev = {.uid = "pwm1", .kind = TS_DEV_PWM};

	zassert_equal(ts_safety_register_channel(&pwm_ch), TS_OK);
	zassert_equal(ts_hal_register_dev(&pwm_dev), TS_OK);

	uint8_t inst = (uint8_t)(ts_hal_dev_count() - 1);
	char cap[32];

	snprintf(cap, sizeof(cap), "pwm:set:%u", inst);
	ts_perm_table_t t;
	ts_ctx_t ctx;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse(cap, &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 3, &t), TS_OK);

	ts_safety_set_link(true);
	zassert_equal(ts_pwm_set(ctx, inst, 7000000U, 500), TS_E_PARAM, "hz 超打包域上限");
	zassert_equal(ts_pwm_set(ctx, inst, 1000U, 1001), TS_E_PARAM, "permille > 1000");
	zassert_equal(ts_pwm_set(ctx, inst, 1000U, 500), TS_OK);
	ts_out_value_t rb;

	zassert_equal(ts_safety_readback("pwm1", &rb), TS_OK);
	zassert_equal(rb.u, ((1000U / 100U) << 16) | 500U, "打包值经安全层往返一致");

	ts_hal_unbind_context(&ctx);
}

/* ---- 实例注册边界（L1）--------------------------------------------------- */

/* ---- input monitor（G3，单元 F）：注入驱动的变化语义 ---------------------- */
static int inp_events;

static void inp_cb(const ts_evt_t *evt, void *user)
{
	ARG_UNUSED(user);
	if (evt->id == TS_EVT_INPUT_CHANGED) {
		inp_events++;
	}
}

ZTEST(framework_hal, test_08_input_monitor_change)
{
	static const ts_hal_dev_desc_t adc_dev = {
		.uid = "inpadc", .kind = TS_DEV_ADC,
	};
	ts_res_t r = ts_hal_register_dev(&adc_dev);

	zassert_equal(r, TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_INPUT_CHANGED, inp_cb, NULL), TS_OK);

	/* 注册表全局索引（前序用例已注册 led/pwm）——扫出 ADC 实例号 */
	uint32_t adc_idx = 0;

	for (uint32_t i = 0; i < ts_hal_dev_count(); i++) {
		if (ts_hal_dev_get(i)->kind == TS_DEV_ADC) {
			adc_idx = i;
			break;
		}
	}

	/* 基线拍（首拍只立基线不发事件） */
	ts_hal_input_test_inject(adc_idx, 100);
	ts_hal_input_poll_once();
	zassert_equal(inp_events, 0, "首拍 = 基线");

	/* 变化 → 事件 */
	ts_hal_input_test_inject(adc_idx, 250);
	ts_hal_input_poll_once();
	zassert_equal(inp_events, 1, "变化即发（传输级语义）");

	/* 不变 → 静默 */
	ts_hal_input_test_inject(adc_idx, 250);
	ts_hal_input_poll_once();
	zassert_equal(inp_events, 1, "不变静默");
}

/* V0.4.0 M5（DR-02）：数字输入纳入监测（传输级 0/1）------------------------- */

ZTEST(framework_hal, test_11_gpio_in_digital_events)
{
	static const ts_hal_dev_desc_t gin_dev = {
		.uid = "inpgio", .kind = TS_DEV_GPIO_IN,
	};
	/* ztest 不保证源码序（test_09 会填满注册表）——顺序无关：优先复用
	 * 既有 GPIO_IN 实例（test_09 的 pad 族即 GPIO_IN），无则注册新例。 */
	uint32_t gin_idx = UINT32_MAX;

	for (uint32_t i = 0; i < ts_hal_dev_count(); i++) {
		if (ts_hal_dev_get(i)->kind == TS_DEV_GPIO_IN) {
			gin_idx = i;
			break;
		}
	}
	if (gin_idx == UINT32_MAX) {
		zassert_equal(ts_hal_register_dev(&gin_dev), TS_OK);
		for (uint32_t i = 0; i < ts_hal_dev_count(); i++) {
			if (strcmp(ts_hal_dev_get(i)->uid, "inpgio") == 0) {
				gin_idx = i;
				break;
			}
		}
	}
	zassert_not_equal(gin_idx, UINT32_MAX, "GPIO_IN 实例就位");
	const int base = inp_events; /* 前序用例计数（差值断言免序耦合） */

	/* 基线拍（0）→ 置位（1）→ 事件；不变 → 静默；回零 → 事件（传输级 0/1） */
	ts_hal_input_test_inject(gin_idx, 0);
	ts_hal_input_poll_once();
	zassert_equal(inp_events - base, 0, "首拍基线");

	ts_hal_input_test_inject(gin_idx, 1);
	ts_hal_input_poll_once();
	zassert_equal(inp_events - base, 1, "数字沿 0→1 即发");

	ts_hal_input_test_inject(gin_idx, 1);
	ts_hal_input_poll_once();
	zassert_equal(inp_events - base, 1, "不变静默");

	ts_hal_input_test_inject(gin_idx, 0);
	ts_hal_input_poll_once();
	zassert_equal(inp_events - base, 2, "数字沿 1→0 即发");
}

ZTEST(framework_hal, test_09_dev_registry_bounds)
{
	zassert_equal(ts_hal_register_dev(NULL), TS_E_PARAM);

	/* 动态填满（不论当前已注册多少） */
	static ts_hal_dev_desc_t fill[CONFIG_TS_HAL_MAX_INSTANCES];
	static char names[CONFIG_TS_HAL_MAX_INSTANCES][16];
	int filled = 0;

	while (ts_hal_dev_count() < CONFIG_TS_HAL_MAX_INSTANCES && filled < CONFIG_TS_HAL_MAX_INSTANCES) {
		snprintf(names[filled], sizeof(names[filled]), "pad%d", filled);
		fill[filled].uid = names[filled];
		fill[filled].kind = TS_DEV_GPIO_IN;
		if (ts_hal_register_dev(&fill[filled]) != TS_OK) break;
		filled++;
	}
	zassert_equal(ts_hal_dev_count(), CONFIG_TS_HAL_MAX_INSTANCES, "filled to max");
	static const ts_hal_dev_desc_t one_more = {.uid = "overflow", .kind = TS_DEV_ADC};
	zassert_equal(ts_hal_register_dev(&one_more), TS_E_NOMEM, "registry full");
}

/* V0.4.0 M2（IR-16）：日志面真实化——裁决 + 全局窗口限速 --------------------- */

ZTEST(framework_hal, test_10_log_perm_and_rate)
{
	ts_perm_table_t t;
	ts_ctx_t ctx;

	/* 未授权：log fail-closed（生产语义——TEST 自动授予只在 appmgr 装载路径，
	 * 直调 api 层如实验证裁决本体） */
	ts_perm_table_init(&t);
	zassert_equal(ts_hal_bind_context(&ctx, 7, &t), TS_OK);
	zassert_equal(ts_log_write(ctx, 3, "x", 1), TS_E_PERM);
	ts_hal_unbind_context(&ctx);

	/* 授权：log:write:0 放行 + 参数校验 */
	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("log:write:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 7, &t), TS_OK);
	zassert_equal(ts_log_write(ctx, 3, "m", 1), TS_OK);
	zassert_equal(ts_log_write(ctx, 3, NULL, 1), TS_E_PARAM);
	zassert_equal(ts_log_write(ctx, 3, "e", 0), TS_E_PARAM);

	/* 限速：全局窗口预算打满 → TS_E_BUSY（上面已成功 1 条，补至满额后过界） */
	for (int i = 0; i < CONFIG_TS_LOG_RATE_PER_S - 1; i++) {
		zassert_equal(ts_log_write(ctx, 3, "burst", 5), TS_OK);
	}
	zassert_equal(ts_log_write(ctx, 3, "over", 4), TS_E_BUSY);
	ts_hal_unbind_context(&ctx);
}

/* V0.4.0 M9（DR-15）：kv 裁决 + 读写回 --------------------------------------- */

ZTEST(framework_hal, test_12_kv_adjudication)
{
	ts_perm_table_t t;
	ts_ctx_t ctx;
	const uint8_t blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t out[8];

	/* 未授权：fail-closed（生产语义） */
	ts_perm_table_init(&t);
	zassert_equal(ts_hal_bind_context(&ctx, 9, &t), TS_OK);
	zassert_equal(ts_kv_save(ctx, 0xABC, blob, 8), TS_E_PERM);
	zassert_equal(ts_kv_load(ctx, 0xABC, out, 8), (int32_t)TS_E_PERM);
	ts_hal_unbind_context(&ctx);

	/* 授权 kv:write:0 + kv:read:0 → 读写回（RAM 后端） */
	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("kv:write:0", &t), TS_OK);
	zassert_equal(ts_perm_parse("kv:read:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 9, &t), TS_OK);
	zassert_equal(ts_kv_save(ctx, 0xABC, blob, 8), TS_OK);
	zassert_equal(ts_kv_load(ctx, 0xABC, out, 8), 8);
	zassert_equal(memcmp(out, blob, 8), 0, "读回一致");
	/* 参数校验 */
	zassert_equal(ts_kv_save(ctx, 0xABC, blob, 0), TS_E_PARAM);
	zassert_equal(ts_kv_load(ctx, 0xABC, out, 0), (int32_t)TS_E_PARAM);
	ts_hal_unbind_context(&ctx);
}

ZTEST_SUITE(framework_hal, NULL, NULL, NULL, NULL, NULL);

/* K2（Q-44②/DEC-61②）：音频输出面裁决——av:write 位图加性 + pcm16 绑定门 */
ZTEST(framework_hal, test_13_av_write_adjudication)
{
	ts_perm_table_t t;
	ts_ctx_t ctx;
	static uint8_t frame[640];

	/* 未授权 = fail-closed（输出新面与既有读面同纪律） */
	ts_perm_table_init(&t);
	zassert_equal(ts_hal_bind_context(&ctx, 9, &t), TS_OK);
	zassert_equal(ts_av_write(ctx, frame, sizeof(frame)), TS_E_PERM);
	ts_hal_unbind_context(&ctx);
	/* av:read 不覆盖 av:write（位图加性——旧 caps 零变化实证） */
	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("av:read:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 9, &t), TS_OK);
	zassert_equal(ts_av_write(ctx, frame, sizeof(frame)), TS_E_PERM);
	ts_hal_unbind_context(&ctx);
	/* av:write + 未绑定 av_fmt = E_STATE（配置面 fail-closed） */
	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("av:write:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 9, &t), TS_OK);
	zassert_equal(ts_av_write(ctx, frame, sizeof(frame)), TS_E_STATE);
	/* 绑定非 pcm16（JPEG）= E_STATE（输出面仅 pcm16——Q-36 语义对称） */
	zassert_equal(ts_av_config_bind(9, TS_AV_FMT_JPEG, 320, 240), TS_OK);
	zassert_equal(ts_av_write(ctx, frame, sizeof(frame)), TS_E_STATE);
	/* 绑定 pcm16 + av:write → 无输出后端（native_sim）= E_IO 如实 */
	zassert_equal(ts_av_config_bind(9, TS_AV_FMT_PCM16, 16, 0), TS_OK);
	zassert_equal(ts_av_write(ctx, frame, sizeof(frame)), TS_E_IO);
	/* 参数门：非 640B 定长 / NULL */
	zassert_equal(ts_av_write(ctx, frame, 639), TS_E_PARAM);
	zassert_equal(ts_av_write(ctx, NULL, 640), TS_E_PARAM);
	ts_hal_unbind_context(&ctx);
}
