/* SPDX-License-Identifier: Apache-2.0 */
/* ts_api_v1 的 wasm 导入面（DEC-25 最小导入面 = 权限硬边界落点；
 * LLD-ts-hal §3 + LLD-ts-appmgr §5）。
 * 防伪造（LLD-ts-common §3.1）：调用者 ctx 由宿主经 exec_env user_data 注入
 * （runtime.c），wasm 传入的 ctx 位仅作不透明占位——natives 只信注入值。
 * 权限裁决：全部经 ts-hal（perm 表 + TS_EVT_PERM_DENIED 留痕 = 合同 10）。
 * 已知偏差（LLD-ts-appmgr 修订登记）：WAMR natives 为全局注册，
 * "未授权符号链接期不存在"的结构化装配留待 WAMR per-instance 支持；
 * 调用期拒绝不削弱合同 10（越权拒绝 + 留痕完整）。
 * V1 符号集 = ts-hal 已实现子集（gpio/pwm/adc/time/log + fs×4〔MD1.2e〕
 * + av 采集/发布〔MD1.2g，DEC-47③⑥〕）；msg/power_set 随对应 hal API
 * 实装后追加。
 * 读类返回约定：>= 0 = 值；< 0 = ts_res_t 错误码（err.h 负数）。 */
#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h> /* P1-1：ts_appmgr_msg_*（APP 间消息路由） */
#include <ts/hal.h>
#include <ts/power.h> /* P1-3：ts_power_request/slot_get（供电槽 native 面） */
#include <ts/safety.h> /* P1-3：readback */
#include <wasm_export.h>
#ifdef CONFIG_TS_STORE_KV
#include <ts/store.h> /* TS_KV_BLOB_MAX（V0.4.0 M9） */
#endif

struct ts_app_rt; /* runtime.c 内部类型（仅指针身份） */
extern ts_ctx_t ts_app_rt_ctx(struct ts_app_rt *rt);
extern const char *ts_app_rt_idstr(struct ts_app_rt *rt); /* M9：kv 哈希源 */

static ts_ctx_t ctx_of(wasm_exec_env_t env)
{
	return ts_app_rt_ctx((struct ts_app_rt *)wasm_runtime_get_user_data(env));
}

static int32_t native_gpio_write(wasm_exec_env_t env, uint32_t ctx_opaque,
				 uint32_t inst, uint32_t v)
{
	ARG_UNUSED(ctx_opaque);
	return (int32_t)ts_gpio_write(ctx_of(env), (uint8_t)inst, v != 0);
}

static int32_t native_gpio_read(wasm_exec_env_t env, uint32_t ctx_opaque,
				uint32_t inst)
{
	ARG_UNUSED(ctx_opaque);
	bool v = false;
	ts_res_t r = ts_gpio_read(ctx_of(env), (uint8_t)inst, &v);

	return (r == TS_OK) ? (v ? 1 : 0) : (int32_t)r;
}

static int32_t native_pwm_set(wasm_exec_env_t env, uint32_t ctx_opaque,
			      uint32_t inst, uint32_t hz, uint32_t permille)
{
	ARG_UNUSED(ctx_opaque);
	return (int32_t)ts_pwm_set(ctx_of(env), (uint8_t)inst, hz,
				   (uint16_t)permille);
}

static int32_t native_adc_read(wasm_exec_env_t env, uint32_t ctx_opaque,
			       uint32_t inst)
{
	ARG_UNUSED(ctx_opaque);
	int32_t mv = 0;
	ts_res_t r = ts_adc_read(ctx_of(env), (uint8_t)inst, &mv);

	return (r == TS_OK) ? mv : (int32_t)r;
}

static int64_t native_time_ms(wasm_exec_env_t env, uint32_t ctx_opaque)
{
	ARG_UNUSED(ctx_opaque);
	return (int64_t)ts_time_ms_api(ctx_of(env));
}

static int32_t native_log_write(wasm_exec_env_t env, uint32_t ctx_opaque,
				uint32_t lvl, uint32_t msg_off, uint32_t len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	char buf[96];

	if (len == 0 || len >= sizeof(buf) ||
	    !wasm_runtime_validate_app_addr(inst, msg_off, len)) {
		return TS_E_PARAM;
	}
	const char *src = (const char *)wasm_runtime_addr_app_to_native(inst, msg_off);

	memcpy(buf, src, len);
	buf[len] = '\0';
	/* V0.4.0 M2：裁决/限速/前缀收敛到 ts_log_write（IR-16）——native
	 * 仅做 wasm 地址边界校验与拷贝，唯一日志路径经 ts-hal。 */
	return ts_log_write(ctx_of(env), (uint8_t)lvl, buf, len);
}

/* ---- ts-fs natives（DEC-47④，MD1.2e）--------------------------------
 * wasm 指针参数一律 validate_app_addr 后经 addr_app_to_native 落主机指针
 * （native_log_write 同纪律）。读类返回：>= 0 = 字节数；< 0 = ts_res_t。 */

/* ---- ts-kv natives（V0.4.0 M9，DR-15）------------------------------------
 * key = 本 APP 串身份的 FNV-1a 32（boot 装载期注入 rt.app_id_str——跨升级/
 * 回滚归属不变）；经 ts-hal kv 权限裁决（kv:read:0 / kv:write:0）。 */
#ifdef CONFIG_TS_STORE_KV
static uint32_t fnv1a(const char *s)
{
	uint32_t h = 0x811C9DC5U;

	while (*s != '\0') {
		h ^= (uint8_t)*s++;
		h *= 0x01000193U;
	}
	return h;
}

static int32_t native_kv_save(wasm_exec_env_t env, uint32_t ctx_opaque,
			      uint32_t off, uint32_t len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (len == 0 || len > TS_KV_BLOB_MAX ||
	    !wasm_runtime_validate_app_addr(inst, off, len)) {
		return TS_E_PARAM;
	}
	const uint8_t *src = (const uint8_t *)wasm_runtime_addr_app_to_native(inst, off);
	struct ts_app_rt *rt = wasm_runtime_get_user_data(env);

	return ts_kv_save(ctx_of(env), fnv1a(ts_app_rt_idstr(rt)), src,
			  (uint16_t)len);
}

static int32_t native_kv_load(wasm_exec_env_t env, uint32_t ctx_opaque,
			      uint32_t off, uint32_t max)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (max == 0 || max > TS_KV_BLOB_MAX ||
	    !wasm_runtime_validate_app_addr(inst, off, max)) {
		return TS_E_PARAM;
	}
	uint8_t *dst = (uint8_t *)wasm_runtime_addr_app_to_native(inst, off);
	struct ts_app_rt *rt = wasm_runtime_get_user_data(env);

	return ts_kv_load(ctx_of(env), fnv1a(ts_app_rt_idstr(rt)), dst,
			  (uint16_t)max);
}
#endif

#ifdef CONFIG_TS_HAL_FS
static char fs_path_buf[96];

static const char *fs_path_in(wasm_module_inst_t inst, uint32_t off, uint32_t len)
{
	if (len == 0 || len >= sizeof(fs_path_buf) ||
	    !wasm_runtime_validate_app_addr(inst, off, len)) {
		return NULL;
	}
	const char *src = (const char *)wasm_runtime_addr_app_to_native(inst, off);

	memcpy(fs_path_buf, src, len);
	fs_path_buf[len] = '\0';
	if (memchr(fs_path_buf, '\0', len) != NULL) {
		return NULL; /* 内嵌 NUL = 非法路径 */
	}
	return fs_path_buf;
}

static int32_t native_fs_read(wasm_exec_env_t env, uint32_t ctx_opaque,
			      uint32_t path_off, uint32_t path_len,
			      uint32_t off, uint32_t buf_off, uint32_t cap)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	const char *path = fs_path_in(inst, path_off, path_len);
	uint8_t *host;

	if (path == NULL || cap == 0 || cap > 0xFFFF ||
	    !wasm_runtime_validate_app_addr(inst, buf_off, cap)) {
		return TS_E_PARAM;
	}
	host = (uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_off);
	uint16_t n = (uint16_t)cap;
	ts_res_t r = ts_fs_read(ctx_of(env), path, off, host, &n);

	return (r == TS_OK) ? (int32_t)n : (int32_t)r;
}

static int32_t native_fs_write(wasm_exec_env_t env, uint32_t ctx_opaque,
			       uint32_t path_off, uint32_t path_len,
			       uint32_t off, uint32_t data_off, uint32_t len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	const char *path = fs_path_in(inst, path_off, path_len);
	const uint8_t *host;

	if (path == NULL || len > 0xFFFF ||
	    (len > 0 && !wasm_runtime_validate_app_addr(inst, data_off, len))) {
		return TS_E_PARAM;
	}
	host = (const uint8_t *)wasm_runtime_addr_app_to_native(inst, data_off);
	return (int32_t)ts_fs_write(ctx_of(env), path, off, host, (uint16_t)len);
}

static int32_t native_fs_list(wasm_exec_env_t env, uint32_t ctx_opaque,
			      uint32_t dir_off, uint32_t dir_len,
			      uint32_t out_off, uint32_t cap)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	const char *dir = fs_path_in(inst, dir_off, dir_len);
	char *host;

	if (dir == NULL || cap < 2 || cap > 0xFFFF ||
	    !wasm_runtime_validate_app_addr(inst, out_off, cap)) {
		return TS_E_PARAM;
	}
	host = (char *)wasm_runtime_addr_app_to_native(inst, out_off);
	uint16_t n = (uint16_t)cap;
	ts_res_t r = ts_fs_list(ctx_of(env), dir, host, n, &n);

	return (r == TS_OK) ? (int32_t)n : (int32_t)r;
}

static int32_t native_fs_delete(wasm_exec_env_t env, uint32_t ctx_opaque,
				uint32_t path_off, uint32_t path_len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	const char *path = fs_path_in(inst, path_off, path_len);

	if (path == NULL) {
		return TS_E_PARAM;
	}
	return (int32_t)ts_fs_delete(ctx_of(env), path);
}
#endif /* CONFIG_TS_HAL_FS */

/* ---- ts-av natives（DEC-47③⑥，MD1.2g）------------------------------
 * 读类；权限/配置裁决在 hal/av.c（av:read:0 + av_fmt/av_w/av_h 绑定）。 */
#ifdef CONFIG_TS_HAL_AV
static int32_t native_av_capture(wasm_exec_env_t env, uint32_t ctx_opaque,
				 uint32_t buf_off, uint32_t cap)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (cap == 0 || !wasm_runtime_validate_app_addr(inst, buf_off, cap)) {
		return TS_E_PARAM;
	}
	uint8_t *dst = (uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_off);
	uint32_t len = 0;
	ts_res_t r = ts_av_capture(ctx_of(env), dst, cap, &len);

	return (r == TS_OK) ? (int32_t)len : (int32_t)r;
}

/* K2（Q-44②/DEC-61②）：音频输出面——pcm16 定长帧（640B）阻塞写；
 * 权限 av:write + pcm16 绑定在 hal/av.c 裁决。 */
static int32_t native_av_write(wasm_exec_env_t env, uint32_t ctx_opaque,
			       uint32_t buf_off, uint32_t len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (len == 0 || !wasm_runtime_validate_app_addr(inst, buf_off, len)) {
		return TS_E_PARAM;
	}
	const uint8_t *src =
		(const uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_off);

	return (int32_t)ts_av_write(ctx_of(env), src, len);
}

#ifdef CONFIG_TS_NET
static int32_t native_net_publish(wasm_exec_env_t env, uint32_t ctx_opaque,
				  uint32_t fid, uint32_t cid, uint32_t n,
				  uint32_t buf_off, uint32_t len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (len == 0 || !wasm_runtime_validate_app_addr(inst, buf_off, len)) {
		return TS_E_PARAM;
	}
	const uint8_t *src = (const uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_off);

	return (int32_t)ts_av_publish(ctx_of(env), fid, cid, n, src, len);
}
#endif /* CONFIG_TS_NET */
#endif /* CONFIG_TS_HAL_AV */

/* ---- ts-thread natives（M19a，DEC-55/Q-37 A）---------------------------
 * 宿主侧线程编排：ts_thread_start(ctx, name, name_len, arg) → 句柄；
 * ts_thread_join(ctx, handle) → worker 返回值（>=0）或 ts_res_t。
 * worker = 一次性任务（入口返回即线程结束）；每线程指令预算在宿主回调
 * 先行设定（L2 硬上界）；跨线程数据 = 共享线性内存 + 原子内建。 */
#ifdef CONFIG_TS_APP_THREADS
extern int ts_app_rt_thread_start(struct ts_app_rt *r, const char *name,
				  uint32_t arg, uint32_t *handle);
extern int ts_app_rt_thread_join(struct ts_app_rt *r, uint32_t handle);

static int32_t native_thread_start(wasm_exec_env_t env, uint32_t ctx_opaque,
				   uint32_t name_off, uint32_t name_len,
				   uint32_t arg)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	char name[24];

	if (name_len == 0 || name_len >= sizeof(name) ||
	    !wasm_runtime_validate_app_addr(inst, name_off, name_len)) {
		return TS_E_PARAM;
	}
	const char *src =
		(const char *)wasm_runtime_addr_app_to_native(inst, name_off);

	memcpy(name, src, name_len);
	name[name_len] = '\0';
	uint32_t handle = 0;
	int r = ts_app_rt_thread_start(
		(struct ts_app_rt *)wasm_runtime_get_user_data(env), name,
		arg, &handle);

	return (r == TS_OK) ? (int32_t)handle : (int32_t)r;
}

static int32_t native_thread_join(wasm_exec_env_t env, uint32_t ctx_opaque,
				  uint32_t handle)
{
	ARG_UNUSED(ctx_opaque);
	return (int32_t)ts_app_rt_thread_join(
		(struct ts_app_rt *)wasm_runtime_get_user_data(env), handle);
}
#endif /* CONFIG_TS_APP_THREADS */

/* wasm 导入符号表（namespace "env"；签名 = wasm 参数/返回类型） */


/* P1-1（HLD §3.4）：APP 间消息——框架路由唯一互访通道（DEC-04）。
 * 发送授权 = msg:send:<target> 名单（perm 层）；接收被动取件。
 * 面 = TS_NET（APP 间消息走 appmgr 队列，不依赖 av 面）。 */
#ifdef CONFIG_TS_NET
static int32_t native_msg_send(wasm_exec_env_t env, uint32_t ctx_opaque,
			       uint32_t target_off, uint32_t target_len,
			       uint32_t data_off, uint32_t len)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (target_len == 0 || target_len >= TS_PERM_MSG_NAME_MAX ||
	    !wasm_runtime_validate_app_addr(inst, target_off, target_len)) {
		return TS_E_PARAM;
	}
	char target[TS_PERM_MSG_NAME_MAX];
	const char *t = (const char *)wasm_runtime_addr_app_to_native(inst, target_off);

	memcpy(target, t, target_len);
	target[target_len] = '\0';
	if (memchr(target, '\0', target_len) != NULL) {
		return TS_E_PARAM;
	}
	if (len > TS_APPMGR_MSG_MAX ||
	    (len > 0 && !wasm_runtime_validate_app_addr(inst, data_off, len))) {
		return TS_E_PARAM;
	}
	const uint8_t *data = (const uint8_t *)wasm_runtime_addr_app_to_native(inst, data_off);

	return (int32_t)ts_appmgr_msg_send(ctx_of(env), target, data, len);
}

static int32_t native_msg_recv(wasm_exec_env_t env, uint32_t ctx_opaque,
			       uint32_t from_off, uint32_t from_cap,
			       uint32_t buf_off, uint32_t cap)
{
	ARG_UNUSED(ctx_opaque);
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (cap == 0 || cap > TS_APPMGR_MSG_MAX ||
	    !wasm_runtime_validate_app_addr(inst, buf_off, cap) ||
	    (from_cap > 0 && !wasm_runtime_validate_app_addr(inst, from_off, from_cap))) {
		return TS_E_PARAM;
	}
	char *from = (from_cap > 0)
		? (char *)wasm_runtime_addr_app_to_native(inst, from_off) : NULL;
	uint8_t *buf = (uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_off);
	uint32_t len = 0;
	ts_res_t r = ts_appmgr_msg_recv(ctx_of(env), from, from_cap, buf, cap, &len);

	return (r == TS_OK) ? (int32_t)len : (int32_t)r;
}
#endif /* CONFIG_TS_NET */

/* P1-3（HLD §3.6 · DEC-03/合同 7）：受控供电槽 native 面——ts-power 层
 * 全链（perm power:set:slot + 预算联动 POWER_BUDGET + 唯一写路径 commit）
 * 的 wasm 暴露；get = safety readback（en 返回值 0/1，ma 出参）。
 * 面 = TS_POWER（与 av/net 面正交）。 */
#ifdef CONFIG_TS_POWER
static int32_t native_power_set(wasm_exec_env_t env, uint32_t ctx_opaque,
				uint32_t slot, uint32_t on, uint32_t ma)
{
	ARG_UNUSED(ctx_opaque);
	return (int32_t)ts_power_request(ctx_of(env), (uint8_t)slot,
					 on != 0, ma);
}

static int32_t native_power_get(wasm_exec_env_t env, uint32_t ctx_opaque,
				uint32_t slot, uint32_t ma_off)
{
	ARG_UNUSED(ctx_opaque);
	const ts_pwr_slot_t *s = ts_power_slot_get((uint8_t)slot);

	if (s == NULL) {
		return TS_E_NOTFOUND;
	}
	ts_out_value_t rb;
	ts_res_t r = ts_safety_readback(s->uid, &rb);

	if (r != TS_OK) {
		return (int32_t)r;
	}
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);

	if (!wasm_runtime_validate_app_addr(inst, ma_off, sizeof(uint32_t))) {
		return TS_E_PARAM;
	}
	uint32_t *out = (uint32_t *)wasm_runtime_addr_app_to_native(inst, ma_off);

	*out = rb.pwr.ma;
	return rb.pwr.en ? 1 : 0;
}
#endif /* CONFIG_TS_POWER */

static NativeSymbol ts_native_syms[] = {
	{"ts_gpio_write", native_gpio_write, "(iii)i", NULL},
	{"ts_gpio_read", native_gpio_read, "(ii)i", NULL},
	{"ts_pwm_set", native_pwm_set, "(iiii)i", NULL},
	{"ts_adc_read", native_adc_read, "(ii)i", NULL},
	{"ts_time_ms", native_time_ms, "(i)I", NULL},
	{"ts_log_write", native_log_write, "(iiii)i", NULL},
#ifdef CONFIG_TS_STORE_KV
	{"ts_kv_save", native_kv_save, "(iii)i", NULL},
	{"ts_kv_load", native_kv_load, "(iii)i", NULL},
#endif
#ifdef CONFIG_TS_HAL_FS
	{"ts_fs_read", native_fs_read, "(iiiiii)i", NULL},
	{"ts_fs_write", native_fs_write, "(iiiiii)i", NULL},
	{"ts_fs_list", native_fs_list, "(iiiii)i", NULL},
	{"ts_fs_delete", native_fs_delete, "(iiii)i", NULL},
#endif
#ifdef CONFIG_TS_HAL_AV
	{"ts_av_capture", native_av_capture, "(iii)i", NULL},
	{"ts_av_write", native_av_write, "(iii)i", NULL}, /* K2：av:write 输出面 */
#ifdef CONFIG_TS_NET
	{"ts_net_publish", native_net_publish, "(iiiiii)i", NULL},
#endif
#endif
#ifdef CONFIG_TS_NET
	{"ts_msg_send", native_msg_send, "(iiiiii)i", NULL}, /* P1-1：msg:send 名单授权 */
	{"ts_msg_recv", native_msg_recv, "(iiiii)i", NULL},
#endif
#ifdef CONFIG_TS_POWER
	{"ts_power_set", native_power_set, "(iiii)i", NULL}, /* P1-3：power:set:slot 权限 + 预算联动 */
	{"ts_power_get", native_power_get, "(iiii)i", NULL},
#endif
#ifdef CONFIG_TS_APP_THREADS
	{"ts_thread_start", native_thread_start, "(iiii)i", NULL},
	{"ts_thread_join", native_thread_join, "(ii)i", NULL},
#endif
};

bool ts_app_natives_register(void)
{
	static bool done;

	if (done) {
		return true;
	}
	done = wasm_runtime_register_natives(
		"env", ts_native_syms,
		sizeof(ts_native_syms) / sizeof(ts_native_syms[0]));
	return done;
}
