/* SPDX-License-Identifier: Apache-2.0 */
/* R05 Wasm 闭环测试：签名好包 → load（绑定 owner）→ call（调用方须为
 * owner；导出 ABI 白名单；代表 native 身份裁决经 VM 真路径）→ 超时停止；
 * 坏签名拒装载。夹具 = 手写 wasm 字节（本机无 wat2wasm；WAT 对照注释）。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/appstore.h>
#include <entelea/d12.h>
#include <entelea/delivery.h>
#include <entelea/package.h>
#include <entelea/principal.h>
#include <entelea/storage.h>
#include <entelea/wasm_host.h>
#include <wasm_export.h>
#include "crypto/tweetnacl.h"

static int observed_instruction_limit;

void __real_wasm_runtime_set_instruction_count_limit(wasm_exec_env_t env, int limit);

void __wrap_wasm_runtime_set_instruction_count_limit(wasm_exec_env_t env, int limit)
{
	observed_instruction_limit = limit;
	__real_wasm_runtime_set_instruction_count_limit(env, limit);
}

static const unsigned char test_root_sk[64] = {
#include "test_root_sk.inc"
};

/* 旧 wamr/app 固定夹具 sample.wasm（103B；导出 health_ping/on_input，
 * 无外部导入——闭环最小面）。 */
static const uint8_t WASM_SAMPLE[] = {
#include "sample_wasm.inc"
};

/*
 * 代表 native 夹具（字节逐条对应下列 WAT）：
 *   (module
 *     (import "entelea" "host_probe" (func $probe (param i32) (result i32)))
 *     (func (export "run") (param i32) (result i32)
 *       (call $probe (local.get 0)))
 *     (func (export "spin") (param i32) (result i32)      ;; 恒死循环
 *       (loop $l
 *         (br_if $l (i32.ge_s (local.get 0) (i32.const 0)))
 *         (local.set 0 (i32.sub (local.get 0) (i32.const 1)))
 *         (br $l))
 *       (local.get 0)))
 * RF1：原夹具 WASM_RUN_UNUSED 段长与内容不符（类型段声明 17 实为 16），
 * 从未能装载——VM→native 身份路径实际未被验证过。现字节按 WAT 重新
 * 组装（段长逐段自洽），由 test_rf1_* 经 WAMR 装载器实际执行。
 */
static const uint8_t WASM_PROBE[] = {
	0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x06,0x01,0x60,
	0x01,0x7f,0x01,0x7f,0x02,0x16,0x01,0x07,0x65,0x6e,0x74,0x65,
	0x6c,0x65,0x61,0x0a,0x68,0x6f,0x73,0x74,0x5f,0x70,0x72,0x6f,
	0x62,0x65,0x00,0x00,0x03,0x03,0x02,0x00,0x00,0x07,0x0e,0x02,
	0x03,0x72,0x75,0x6e,0x00,0x01,0x04,0x73,0x70,0x69,0x6e,0x00,
	0x02,0x0a,0x20,0x02,0x06,0x00,0x20,0x00,0x10,0x00,0x0b,0x17,
	0x00,0x03,0x40,0x20,0x00,0x41,0x00,0x4e,0x0d,0x00,0x20,0x00,
	0x41,0x01,0x6b,0x21,0x00,0x0c,0x00,0x0b,0x20,0x00,0x0b,
};

static const uint8_t MAN[] = {0xa1, 0x63, 'a', 'p', 'p', 0x01};

/* 最近一次 stage_signed 写入字节的独立 sha256 与总长（整包文件身份 =
 * Forge package_digest 口径；与被测 entelea_pkg_file_digest 对拍）。 */
static uint8_t staged_dg[32];
static uint32_t staged_len;

static void stage_signed(uint8_t slot, const uint8_t *wasm, uint32_t wlen)
{
	uint8_t pkg[1024];
	uint32_t n = 0;

	pkg[n++]='T';pkg[n++]='S';pkg[n++]='A';pkg[n++]='P';
	pkg[n++]=0;pkg[n++]=2;
	pkg[n++]=0;pkg[n++]=0;pkg[n++]=0;pkg[n++]=(uint8_t)sizeof(MAN);
	pkg[n++]=0;pkg[n++]=0;pkg[n++]=(uint8_t)(wlen>>8);pkg[n++]=(uint8_t)wlen;
	pkg[n++]=0;pkg[n++]=1;
	uint32_t st[8];uint8_t dg[32];
	entelea_sha256_stream_init(st);
	entelea_sha256_stream_update(st,MAN,sizeof(MAN));
	entelea_sha256_stream_update(st,wasm,wlen);
	entelea_sha256_stream_final(st,dg);
	memcpy(pkg+n,dg,32);n+=32;
	memcpy(pkg+n,MAN,sizeof(MAN));n+=sizeof(MAN);
	memcpy(pkg+n,wasm,wlen);n+=wlen;
	uint8_t payload[512];
	uint32_t plen=sizeof(MAN)+wlen;
	memcpy(payload,MAN,sizeof(MAN));memcpy(payload+sizeof(MAN),wasm,wlen);
	static uint8_t sm[64+22+512];
	memcpy(sm,test_root_sk+32,64);
	size_t tail=64;
	sm[tail++]=0x84;sm[tail++]=0x6a;
	memcpy(sm+tail,"Signature1",10);tail+=10;
	sm[tail++]=0x43;sm[tail++]=0xa1;sm[tail++]=0x01;sm[tail++]=0x27;sm[tail++]=0x40;
	sm[tail++]=0x58;sm[tail++]=(uint8_t)plen;
	memcpy(sm+tail,payload,plen);tail+=plen;
	static uint8_t sb[64+512];
	unsigned long long sl=0;
	crypto_sign(sb,&sl,sm+64,tail-64,test_root_sk);
	zassert_equal(entelea_pkg_stage_begin(slot),ENTELEA_OK);
	zassert_equal(entelea_pkg_stage_chunk(slot,0,pkg,n),ENTELEA_OK);
	/* 追加 COSE_Sign1：tag18 + array4 + phdr + uhdr + payload(内嵌副本) + sig */
	uint8_t tail_b[16];
	uint32_t tn=0;
	tail_b[tn++]=0xd2;tail_b[tn++]=0x84;
	tail_b[tn++]=0x43;tail_b[tn++]=0xa1;tail_b[tn++]=0x01;tail_b[tn++]=0x27;
	tail_b[tn++]=0xa0;
	tail_b[tn++]=0x58;tail_b[tn++]=(uint8_t)plen; /* payload bstr 头 */
	zassert_equal(entelea_pkg_stage_chunk(slot,n,tail_b,tn),ENTELEA_OK);
	zassert_equal(entelea_pkg_stage_chunk(slot,n+tn,payload,plen),ENTELEA_OK); /* 内嵌副本 */
	uint8_t sigh[2]={0x58,64};

	zassert_equal(entelea_pkg_stage_chunk(slot,n+tn+plen,sigh,2),ENTELEA_OK);
	zassert_equal(entelea_pkg_stage_chunk(slot,n+tn+plen+2,sb,64),ENTELEA_OK);

	uint32_t fs[8];

	entelea_sha256_stream_init(fs);
	entelea_sha256_stream_update(fs, pkg, n);
	entelea_sha256_stream_update(fs, tail_b, tn);
	entelea_sha256_stream_update(fs, payload, plen);
	entelea_sha256_stream_update(fs, sigh, 2);
	entelea_sha256_stream_update(fs, sb, 64);
	entelea_sha256_stream_final(fs, staged_dg);
	staged_len = n + tn + plen + 2 + 64;
}

static uint64_t d12_fake_now; /* r16：d12 冷却窗面（wasm_allow） */

static entelea_result_t d12_fake_read_ms(void *user, uint64_t *out)
{
	*out = *(uint64_t *)user;
	return ENTELEA_OK;
}

/* ---- RF1 G3 夹具与注入（审查 P1-7/P1-8/P2-13 回归） ---- */

/*
 * 导出 ABI 夹具（字节逐条对应下列 WAT；仅 k/id 属白名单）：
 *   (module
 *     (func (export "k") (result i32) (i32.const 42))
 *     (func (export "id") (param i32) (result i32) (local.get 0))
 *     (func (export "w64") (result i64) (i64.const 0x1122334455667788))
 *     (func (export "two") (param i32 i32) (result i32)
 *       (i32.add (local.get 0) (local.get 1)))
 *     (func (export "f") (param f32) (result i32) (i32.const 5))
 *     (func (export "v") (param i32)))
 */
static const uint8_t WASM_ABI[] = {
	0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x1d,0x06,0x60,
	0x00,0x01,0x7f,0x60,0x01,0x7f,0x01,0x7f,0x60,0x00,0x01,0x7e,
	0x60,0x02,0x7f,0x7f,0x01,0x7f,0x60,0x01,0x7d,0x01,0x7f,0x60,
	0x01,0x7f,0x00,0x03,0x07,0x06,0x00,0x01,0x02,0x03,0x04,0x05,
	0x07,0x1e,0x06,0x01,0x6b,0x00,0x00,0x02,0x69,0x64,0x00,0x01,
	0x03,0x77,0x36,0x34,0x00,0x02,0x03,0x74,0x77,0x6f,0x00,0x03,
	0x01,0x66,0x00,0x04,0x01,0x76,0x00,0x05,0x0a,0x28,0x06,0x04,
	0x00,0x41,0x2a,0x0b,0x04,0x00,0x20,0x00,0x0b,0x0c,0x00,0x42,
	0x88,0xef,0x99,0xab,0xc5,0xe8,0x8c,0x91,0x11,0x0b,0x07,0x00,
	0x20,0x00,0x20,0x01,0x6a,0x0b,0x04,0x00,0x41,0x05,0x0b,0x02,
	0x00,0x0b,
};

/* natives 注册失败注入（链接 --wrap；见 CMakeLists）。 */
static bool fail_register;
bool __real_wasm_runtime_register_natives(const char *module_name,
					   NativeSymbol *native_symbols,
					   uint32_t n_native_symbols);

bool __wrap_wasm_runtime_register_natives(const char *module_name,
					   NativeSymbol *native_symbols,
					   uint32_t n_native_symbols)
{
	if (fail_register) {
		return false;
	}
	return __real_wasm_runtime_register_natives(module_name, native_symbols,
						    n_native_symbols);
}

/* 读失败注入后端（包裹当前后端；写/擦直通）。精确匹配 (part,off,len)
 * 而非按读序号——信任表首读懒加载并缓存，读序号随前序调用漂移。 */
static entelea_store_ops_t rd_base;
static bool rd_mounted;
static struct {
	bool fail_all;
	entelea_store_part_t part;
	uint32_t off;
	uint32_t len;
	uint32_t hits; /* 精确匹配读命中次数（失败确由目标读触发） */
} rd;

static entelea_result_t rd_read(entelea_store_part_t part, uint32_t off,
				void *buf, uint32_t len)
{
	if (rd.fail_all) {
		return ENTELEA_E_IO;
	}
	if (part == rd.part && off == rd.off && len == rd.len) {
		rd.hits++;
		return ENTELEA_E_IO;
	}
	return rd_base.read(part, off, buf, len);
}

static entelea_result_t rd_write(entelea_store_part_t part, uint32_t off,
				 const void *buf, uint32_t len)
{
	return rd_base.write(part, off, buf, len);
}

static entelea_result_t rd_erase(entelea_store_part_t part)
{
	return rd_base.erase(part);
}

static entelea_result_t rd_erase_off(entelea_store_part_t part, uint32_t off,
				     uint32_t len)
{
	return rd_base.erase_off(part, off, len);
}

static uint32_t rd_size(entelea_store_part_t part)
{
	return rd_base.size(part);
}

/* fail_all=false 时只拒 (part,off,len) 精确匹配的读。 */
static void rd_mount(bool fail_all, entelea_store_part_t part, uint32_t off,
		     uint32_t len)
{
	static const entelea_store_ops_t ops = {
		.read = rd_read, .write = rd_write, .erase = rd_erase,
		.erase_off = rd_erase_off, .size = rd_size,
	};

	zassert_false(rd_mounted);
	rd.fail_all = fail_all;
	rd.part = part;
	rd.off = off;
	rd.len = len;
	rd.hits = 0;
	rd_base = entelea_store_backend;
	zassert_equal(entelea_storage_mount(&ops), ENTELEA_OK);
	rd_mounted = true;
}

static void rd_unmount(void)
{
	zassert_equal(entelea_storage_mount(&rd_base), ENTELEA_OK);
	rd_mounted = false;
}

static void wasm_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (rd_mounted) {
		(void)entelea_storage_mount(&rd_base);
		rd_mounted = false;
	}
	fail_register = false;
}

/* RF1：每例独立复位（原 suite 级 setup 只跑一次——用例间靠字母序隐式
 * 依赖）；after 恢复注入后端与注册失败开关，断言中止不殃及后续用例。 */
static void wasm_before(void *fixture)
{
	static entelea_clock_t d12_clock;

	ARG_UNUSED(fixture);
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_wasm_test_reset();
	entelea_perm_test_reset();
	entelea_trial_test_reset();
	observed_instruction_limit = -1;
	d12_fake_now = 0;
	entelea_d12_test_reset();
	zassert_ok(entelea_clock_init(&d12_clock, d12_fake_read_ms,
				      &d12_fake_now, 0xD12));
	zassert_ok(entelea_d12_bind_clock(&d12_clock));
}

/* R12 试用辅助：向 slot 提交（摘要由设备从槽内字节计算；宿主只申请权限、
 * 步数与版本）。 */
static entelea_result_t trial_submit(uint8_t slot, const char *perm,
				     uint32_t budget)
{
	entelea_trial_request_t req;

	memset(&req, 0, sizeof(req));
	req.slot = slot;
	req.version = 1;
	req.budget_steps = budget;
	req.perm_len = (uint8_t)strlen(perm);
	memcpy(req.perm, perm, req.perm_len);
	return entelea_trial_submit(&req);
}

/* 提交并确认 slot 0 当前包：展示对象摘要须等于测试侧独立计算的暂存摘要。 */
static void trial_confirm_slot0(const char *perm, uint32_t budget)
{
	entelea_trial_candidate_t shown;

	zassert_equal(trial_submit(0, perm, budget), ENTELEA_OK);
	entelea_trial_current(&shown);
	zassert_mem_equal(shown.digest, staged_dg, 32,
			  "F4：摘要由设备从槽内字节计算");
	zassert_equal(entelea_trial_confirm(&shown), ENTELEA_OK);
}

/* F3 注入：摘要门读取（第 1 趟）与装载捕获读取（第 2 趟）读的是同一精确区间
 * (SLOT_A, 0, len)。第 2 趟返回前：fail=true 则返回 E_IO；否则翻转 wasm 段内
 * 一个字节。两种情况都须拒绝装载。 */
static entelea_store_ops_t tp_base;
static bool tp_mounted;
static struct {
	uint32_t len;
	uint32_t hits;
	bool fail;
} tp;

static entelea_result_t tp_read(entelea_store_part_t part, uint32_t off,
				void *buf, uint32_t len)
{
	entelea_result_t r = tp_base.read(part, off, buf, len);

	if (r == ENTELEA_OK && part == ENTELEA_PART_SLOT_A && off == 0 &&
	    len == tp.len && ++tp.hits == 2U) {
		if (tp.fail) {
			return ENTELEA_E_IO;
		}
		((uint8_t *)buf)[ENTELEA_TSAP_CONTENT_OFF + sizeof(MAN) + 4U] ^=
			0x01U;
	}
	return r;
}

static void tp_mount(uint32_t len, bool fail)
{
	entelea_store_ops_t tampered;

	zassert_false(tp_mounted);
	tp_base = entelea_store_backend;
	tampered = tp_base;
	tampered.read = tp_read;
	tp.len = len;
	tp.hits = 0;
	tp.fail = fail;
	zassert_equal(entelea_storage_mount(&tampered), ENTELEA_OK);
	tp_mounted = true;
}

static void tp_unmount(void)
{
	if (tp_mounted) {
		zassert_equal(entelea_storage_mount(&tp_base), ENTELEA_OK);
		tp_mounted = false;
	}
}

static entelea_principal_t mk_principal(uint16_t app_id, const char *cap)
{
	entelea_perm_table_t t;
	entelea_principal_t p;

	entelea_perm_table_init(&t);
	if (cap != NULL) {
		zassert_equal(entelea_perm_parse(cap, &t), ENTELEA_OK);
	}
	zassert_equal(entelea_principal_bind(&p, app_id, &t), ENTELEA_OK);
	return p;
}

ZTEST(framework_wasm, test_signed_load_call_stop)
{
	zassert_equal(entelea_wasm_init(), ENTELEA_OK, "WAMR init（池模式）");
	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	entelea_principal_t ctx = mk_principal(1, "gpio:read:0");

	/* 坏签名 slot 拒装载（位腐）。 */
	stage_signed(1, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	uint8_t bad = 0xEE;

	zassert_equal(entelea_pkg_slot_write(1, ENTELEA_TSAP_CONTENT_OFF, &bad, 1),
		      ENTELEA_OK);
	uint32_t app = 0;

	zassert_equal(entelea_wasm_load(1, ctx, &app), ENTELEA_E_INVALID_SIG,
		      "坏签名拒装载");

	/* 好包装载（owner=ctx）+ owner 调用。 */
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(app, 1);
	int32_t ret = -1;

	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, 100, &ret),
		      ENTELEA_OK, "导出函数调用");
	zassert_true(ret >= 0, "health_ping 返回");

	/* 非 owner 调用方。RF1 合同变更（审查 P1-8）：原断言为“未授权
	 * principal 调用成功”——旧 call 把调用方传入的 ctx 直接写成实例身份，
	 * 任何调用方都能替实例挑选 natives 所信的身份。现身份在 load 时绑定，
	 * 调用方≠owner = E_PERM 且不执行、不销毁实例。 */
	entelea_principal_t poor = mk_principal(2, NULL);

	ret = -77;
	zassert_equal(entelea_wasm_call(app, poor, "health_ping", 0, 100, &ret),
		      ENTELEA_E_PERM, "非 owner 调用拒绝");
	zassert_equal(ret, -77, "拒绝不执行");
	zassert_equal(entelea_wasm_instance_count(), 1, "拒绝不销毁实例");

	/* 换包：stop 后重装载另一 slot。 */
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
	stage_signed(1, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	uint32_t app2 = 0;

	zassert_equal(entelea_wasm_load(1, ctx, &app2), ENTELEA_OK, "换包重装载");
	/* 有界停止：预算 0（任何指令即超限）→ E_TIMEOUT + 实例销毁（真死循环
	 * 见 test_rf1_quota_stops_real_infinite_loop）。 */
	zassert_equal(entelea_wasm_call(app2, ctx, "health_ping", 0, 0, &ret),
		      ENTELEA_E_TIMEOUT, "步预算 0 → 有界停止");
	zassert_equal(entelea_wasm_instance_count(), 0, "超时后实例销毁");
	/* 停止后调用 = NOTFOUND（不静默续跑）。 */
	zassert_equal(entelea_wasm_call(app2, ctx, "health_ping", 0, 100, &ret),
		      ENTELEA_E_NOTFOUND);
}

/* E-2 错误面收窄：trap ≠ 配额到期。trap 模块（wasm 二进制手写：
 * (module (func (export "t") (param i32) (result i32) unreachable))
 * ——unreachable 立即 trap，异常串不含 instruction limit → E_IO。 */
static const uint8_t WASM_TRAP[] = {
	0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
	0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
	0x03,0x02,0x01,0x00,
	0x07,0x05,0x01,0x01,0x74,0x00,0x00,
	0x0a,0x05,0x01,0x03,0x00,0x00,0x0b,
};

ZTEST(framework_wasm, test_trap_vs_quota_error_split)
{
	zassert_equal(entelea_wasm_init(), ENTELEA_OK);
	stage_signed(0, WASM_TRAP, sizeof(WASM_TRAP));
	uint32_t app = 0;
	entelea_principal_t ctx = {0};

	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	int32_t ret = -1;

	zassert_equal(entelea_wasm_call(app, ctx, "t", 0, 1000, &ret),
		      ENTELEA_E_IO, "trap → E_IO（非配额面）");
	zassert_equal(entelea_wasm_instance_count(), 0, "trap 后实例销毁");
	/* 判别性对照：同模块预算 0 仍 E_TIMEOUT（配额异常串路径）。 */
	uint32_t app2 = 0;

	stage_signed(1, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(entelea_wasm_load(1, ctx, &app2), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app2, ctx, "health_ping", 0, 0, &ret),
		      ENTELEA_E_TIMEOUT, "配额 0 → E_TIMEOUT（判别对照）");
}

ZTEST_SUITE(framework_wasm, NULL, NULL, wasm_before, wasm_after, NULL);

/* ---- 代表 native 裁决函数（直达钩子；VM 真路径见 test_rf1_owner_*） ---- */
ZTEST(framework_wasm, test_native_probe_principal_gate)
{
	/* 授权 principal（gpio:read:0）→ 1（授权可见）。 */
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:read:0", &t), ENTELEA_OK);
	entelea_principal_t ok_ctx;

	zassert_equal(entelea_principal_bind(&ok_ctx, 1, &t), ENTELEA_OK);
	zassert_equal(entelea_wasm_test_probe_decide(ok_ctx, "gpio:read:0"), 1,
		      "确认授权 + 宿主授权 → 授权可见（Wasm 传参被忽略）");
	zassert_equal(entelea_wasm_test_probe_decide(ok_ctx, NULL),
		      ENTELEA_E_PERM, "无确认授权即拒（A1）");
	zassert_equal(entelea_wasm_test_probe_decide(ok_ctx, "pwm:set:1"),
		      ENTELEA_E_PERM, "确认未含 gpio:read:0 即拒");
	/* 未授权（空权限）→ E_PERM。 */
	entelea_perm_table_t empty;

	entelea_perm_table_init(&empty);
	entelea_principal_t poor;

	zassert_equal(entelea_principal_bind(&poor, 2, &empty), ENTELEA_OK);
	zassert_equal(entelea_wasm_test_probe_decide(poor, "gpio:read:0"),
		      ENTELEA_E_PERM,
		      "注入未授权身份 → E_PERM（不崩溃、不越权）");
	/* 域外句柄 E_PERM；完整匹配的 C 字段不由该断言证明不可伪造。 */
	entelea_principal_t fake = {._handle = 91, .app_id = 0};

	zassert_equal(entelea_wasm_test_probe_decide(fake, "gpio:read:0"),
		      ENTELEA_E_PERM, "伪造句柄 → E_PERM");
}

/* ---- RF1 G3 回归用例 ---- */

/* P1-8：导出 ABI 白名单 ()->i32 / (i32)->i32；其余签名 = E_PARAM 且不执行、
 * 不销毁实例；arg 真实传入（原硬编码 argv={0} 掩盖了 Forge spin(N≥1) 缺陷）。 */
ZTEST(framework_wasm, test_rf1_abi_checked_arg_passed)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;
	int32_t ret = 0;

	stage_signed(0, WASM_ABI, sizeof(WASM_ABI));
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "k", 0, 100, &ret), ENTELEA_OK);
	zassert_equal(ret, 42);
	zassert_equal(entelea_wasm_call(app, ctx, "id", 7, 100, &ret), ENTELEA_OK);
	zassert_equal(ret, 7, "arg 真实传入");
	zassert_equal(entelea_wasm_call(app, ctx, "id", -5, 100, &ret), ENTELEA_OK);
	zassert_equal(ret, -5, "有符号 arg");

	static const char *const rejected[] = {"w64", "two", "f", "v"};

	for (size_t i = 0; i < ARRAY_SIZE(rejected); i++) {
		ret = -77;
		zassert_equal(entelea_wasm_call(app, ctx, rejected[i], 0, 100, &ret),
			      ENTELEA_E_PARAM, "%s 签名拒绝", rejected[i]);
		zassert_equal(ret, -77, "%s 不执行", rejected[i]);
		zassert_equal(entelea_wasm_instance_count(), 1, "%s 不销毁实例",
			      rejected[i]);
	}
	ret = -77;
	zassert_equal(entelea_wasm_call(app, ctx, "k", 5, 100, &ret),
		      ENTELEA_E_PARAM, "无参函数收到非零 arg = 拒绝（不静默丢弃）");
	zassert_equal(ret, -77);
	zassert_equal(entelea_wasm_call(app, ctx, "k", 0, 100, &ret), ENTELEA_OK,
		      "拒绝后实例仍可用");
	zassert_equal(ret, 42);
}

/* P2-13：natives 注册失败 = init 失败（原 bool<0 恒假——失败被吞、宿主无
 * natives 却报 OK）；运行时回滚，重试可恢复。 */
ZTEST(framework_wasm, test_rf1_init_natives_failure_fail_closed)
{
	uint32_t app = 0;
	int32_t ret = 0;
	entelea_principal_t owner = mk_principal(1, "gpio:read:0");

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	fail_register = true;
	zassert_equal(entelea_wasm_init(), ENTELEA_E_IO, "注册失败 → init 失败");
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_E_IO,
		      "未初始化不得装载");
	zassert_equal(entelea_wasm_instance_count(), 0);
	fail_register = false;
	zassert_equal(entelea_wasm_init(), ENTELEA_OK, "回滚后重试成功");
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK, "导入解析成功（natives 已注册）");
	zassert_equal(ret, ENTELEA_E_PERM, "A1：未确认包的 natives 一律拒");
}

/* P1-7：单实例占用时 load 必须先拒（E_BUSY）且零副作用——原实现先卸载
 * 模块再查空槽，活实例引用已释放模块（native_sim 实测 SIGABRT）。 */
ZTEST(framework_wasm, test_rf1_load_busy_keeps_live_instance)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;
	uint32_t app2 = 0xAA;
	int32_t ret = -1;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	stage_signed(1, WASM_ABI, sizeof(WASM_ABI));
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_load(1, ctx, &app2), ENTELEA_E_BUSY, "他槽");
	zassert_equal(entelea_wasm_load(0, ctx, &app2), ENTELEA_E_BUSY, "同槽重载");
	zassert_equal(app2, 0xAA, "拒绝不写出参");
	zassert_equal(entelea_wasm_instance_count(), 1);
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, 100, &ret),
		      ENTELEA_OK, "活实例模块未被卸载");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
	zassert_equal(entelea_wasm_load(1, ctx, &app2), ENTELEA_OK, "stop 后可换包");
	zassert_equal(entelea_wasm_call(app2, ctx, "k", 0, 100, &ret), ENTELEA_OK);
	zassert_equal(ret, 42, "执行的是新槽字节");
}

/* P1-7：介质读错误保持 E_IO——原实现把验签读失败改写为 E_INVALID_SIG、
 * 装载读失败改写为 E_RANGE（运维把介质故障误判为包坏/超限）。 */
ZTEST(framework_wasm, test_rf1_load_io_errors_propagate)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0xAA;
	entelea_tsap_view_t v;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(entelea_wasm_init(), ENTELEA_OK);

	rd_mount(true, ENTELEA_PART_COUNT, 0, 0); /* 验签首读（头）失败 */
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_IO, "验签读错误");
	rd_unmount();

	/* 验签中段（COSE 标签）读失败：原 verify 内部同样改写为 E_INVALID_SIG。 */
	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL), ENTELEA_OK);
	rd_mount(false, ENTELEA_PART_SLOT_A, v.cose_off, 2);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_IO,
		      "COSE 标签读错误");
	zassert_equal(rd.hits, 1);
	rd_unmount();

	/* 验签通过、装载的整包读取失败：先证验签不读整包区间（注入只命中装载读）。
	 * 装载第一趟（门摘要）即整包读取，其失败须原样为 E_IO。 */
	rd_mount(false, ENTELEA_PART_SLOT_A, 0, v.pkg_len);
	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL), ENTELEA_OK);
	zassert_equal(rd.hits, 0, "验签不读整包区间");
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_IO, "装载读错误");
	zassert_equal(rd.hits, 1, "失败确由装载的整包读取触发");
	rd_unmount();
	zassert_equal(app, 0xAA);
	zassert_equal(entelea_wasm_instance_count(), 0);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK, "介质恢复后可装载");
}

/* P1-8：身份在 load 绑定；VM→native 真路径只信 owner。调用方≠owner =
 * E_PERM（不执行、不销毁）；原实现 call 时以调用方 ctx 覆盖实例身份——
 * 低权 owner 的模块可借高权调用方身份越权。 */
ZTEST(framework_wasm, test_rf1_owner_bound_at_load)
{
	entelea_principal_t rich = mk_principal(1, "gpio:read:0");
	entelea_principal_t poor = mk_principal(2, NULL);
	uint32_t app = 0;
	int32_t ret = 0;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	trial_confirm_slot0("gpio:read:0", 1000);
	zassert_equal(entelea_wasm_load(0, poor, &app), ENTELEA_OK);
	ret = -77;
	zassert_equal(entelea_wasm_call(app, rich, "run", 0, 100, &ret),
		      ENTELEA_E_PERM, "高权调用方不得替低权实例换身份");
	zassert_equal(ret, -77, "拒绝不执行");
	zassert_equal(entelea_wasm_instance_count(), 1);
	zassert_equal(entelea_wasm_call(app, poor, "run", 0, 100, &ret), ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "VM 内 native 按 owner 裁决：拒");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	zassert_equal(entelea_wasm_load(0, rich, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, rich, "run", 0xFFFF, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, 1, "VM 内 native 按 owner 裁决：准（Wasm 传参被忽略）");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	entelea_principal_t unbound = {0};

	zassert_equal(entelea_wasm_load(0, unbound, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, unbound, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "未绑定 owner：natives 一律拒");
}

/* 有界停止对真死循环同样成立（原只测预算 0）。 */
ZTEST(framework_wasm, test_rf1_quota_stops_real_infinite_loop)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;
	int32_t ret = -77;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "spin", 3, 10, &ret),
		      ENTELEA_E_TIMEOUT, "死循环 → 配额到期");
	zassert_equal(ret, -77);
	zassert_equal(entelea_wasm_instance_count(), 0, "到期销毁实例");
}

ZTEST(framework_wasm, test_default_instruction_budget_and_explicit_limits)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;
	int32_t ret = -77;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call_default(app, ctx, "spin", 3, &ret),
		      ENTELEA_E_TIMEOUT, "default budget must stop a real loop");
	zassert_equal(observed_instruction_limit, 100000000);
	zassert_equal(ret, -77);
	zassert_equal(entelea_wasm_instance_count(), 0);

	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "run", 0, 1, &ret), ENTELEA_OK);
	zassert_equal(observed_instruction_limit, 1000, "explicit smaller limit");
	zassert_equal(entelea_wasm_call(app, ctx, "run", 0, UINT32_MAX, &ret),
		      ENTELEA_OK);
	zassert_equal(observed_instruction_limit, 100000000, "large limit stays bounded");
	zassert_equal(entelea_wasm_call(app, ctx, "run", 0, 0, &ret),
		      ENTELEA_E_TIMEOUT, "explicit zero keeps its old meaning");
	zassert_equal(observed_instruction_limit, 0);
}

/* F1（D-B）：natives 授权 = 用户确认的权限列表 ∩ 宿主注入的 principal 授权。
 * 确认为空时，即使宿主 principal 有 gpio:read:0 也必须拒绝（原缺口）。 */
ZTEST(framework_wasm, test_f1_native_grant_is_confirmed_intersect_host)
{
	entelea_principal_t owner = mk_principal(1, "gpio:read:0");
	entelea_principal_t nogrant = mk_principal(2, NULL);
	uint32_t app = 0;
	int32_t ret = -77;
	entelea_result_t rc;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));

	trial_confirm_slot0("", 1000);
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	rc = entelea_wasm_call(app, owner, "run", 0, 100, &ret);
	zassert_true(rc == ENTELEA_E_PERM ||
			     (rc == ENTELEA_OK && ret == ENTELEA_E_PERM),
		     "确认为空不得由宿主授权放行: rc=%d ret=%d", (int)rc,
		     (int)ret);
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
	zassert_equal(entelea_trial_end(), ENTELEA_OK);

	trial_confirm_slot0("pwm:set:1", 1000);
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "确认未含 gpio:read:0");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
	zassert_equal(entelea_trial_end(), ENTELEA_OK);

	trial_confirm_slot0("gpio:read:0", 1000);
	zassert_equal(entelea_wasm_load(0, nogrant, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, nogrant, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "宿主无授权即拒");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, 1, "确认与宿主授权同时具备");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}

/* A1：未经确认的包（NONE/非候选）即使宿主有授权，natives 也一律拒绝；
 * 纯计算仍可装载执行（不受试用门管辖，稳定集准入归 D05）。 */
ZTEST(framework_wasm, test_a1_unconfirmed_package_has_no_native_grant)
{
	entelea_principal_t owner = mk_principal(1, "gpio:read:0");
	uint32_t app = 0;
	int32_t ret = -77;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "未确认：natives 一律拒");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}

/* D-C：执行配额 = min(调用方请求, 确认步数, 设备上限 1e8)。 */
ZTEST(framework_wasm, test_dc_budget_is_capped_by_confirmed_steps)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;
	int32_t ret = -77;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	trial_confirm_slot0("", 1000);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, UINT32_MAX,
					&ret),
		      ENTELEA_OK);
	zassert_equal(observed_instruction_limit, 1000, "确认步数封顶大请求");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	trial_confirm_slot0("", 100000000U);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, 1, &ret),
		      ENTELEA_OK);
	zassert_equal(observed_instruction_limit, 1000, "请求更小时取请求");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, UINT32_MAX,
					&ret),
		      ENTELEA_OK);
	zassert_equal(observed_instruction_limit, 100000000,
		      "确认上限不超过设备上限");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}

/* 授权逐次查询：试用结束后，已装载实例的 natives 授权立即失效（不缓存）。 */
ZTEST(framework_wasm, test_grant_refreshed_per_call_after_trial_end)
{
	entelea_principal_t owner = mk_principal(1, "gpio:read:0");
	uint32_t app = 0;
	int32_t ret = -77;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	trial_confirm_slot0("gpio:read:0", 1000);
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, 1, "ACTIVE 期间授权可见");
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	ret = -77;
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "试用结束后授权立即失效");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}

/* F4：提交的摘要由设备从槽内字节计算；空槽与坏包被拒且不改变状态。 */
ZTEST(framework_wasm, test_f4_submit_computes_digest_from_slot_bytes)
{
	entelea_trial_candidate_t shown;
	uint8_t bad = 0xEE;

	zassert_equal(trial_submit(0, "", 1000), ENTELEA_E_INVALID_SIG,
		      "空槽无有效包");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE,
		      "失败不改变状态");

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	zassert_equal(entelea_pkg_slot_write(0, ENTELEA_TSAP_CONTENT_OFF, &bad, 1),
		      ENTELEA_OK);
	zassert_equal(trial_submit(0, "", 1000), ENTELEA_E_INVALID_SIG,
		      "坏包拒提交");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(trial_submit(0, "", 1000), ENTELEA_OK);
	entelea_trial_current(&shown);
	zassert_mem_equal(shown.digest, staged_dg, 32, "设备计算摘要");
}

/* F4（工厂）：声明的包摘要须与设备计算值相等，否则不提交。 */
ZTEST(framework_wasm, test_f4_factory_intake_checks_device_digest)
{
	entelea_factory_candidate_t fc;
	entelea_trial_candidate_t shown;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	memset(&fc, 0, sizeof(fc));
	memcpy(fc.package_digest, staged_dg, sizeof(fc.package_digest));
	fc.runtime_version = 1;
	fc.payload_kind = 0;
	fc.budget_steps = 100000;
	fc.mem_quota_kb = 64;

	fc.package_digest[0] ^= 1U;
	zassert_equal(entelea_factory_intake(&fc, 0), ENTELEA_E_INVALID_SIG,
		      "声明摘要与槽内字节不符");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);
	fc.package_digest[0] ^= 1U;

	zassert_equal(entelea_factory_intake(&fc, 0), ENTELEA_OK);
	entelea_trial_current(&shown);
	zassert_mem_equal(shown.digest, staged_dg, 32);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "intake 只到 PENDING");
}

/* F4（工厂）：Lua 载荷经同一验收面（仅 intake，不默认解释为 wasm）。 */
ZTEST(framework_wasm, test_f4_factory_intake_lua_payload_uses_same_gate)
{
	entelea_factory_candidate_t fc;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	memset(&fc, 0, sizeof(fc));
	memcpy(fc.package_digest, staged_dg, sizeof(fc.package_digest));
	fc.runtime_version = 1;
	fc.payload_kind = 1;
	fc.budget_steps = 100000;
	fc.mem_quota_kb = 64;
	zassert_equal(entelea_factory_intake(&fc, 0), ENTELEA_OK,
		      "Lua payload 合法");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
}

/* F3：门摘要与装载字节来自同一趟读取。门后、捕获前改写槽内 wasm 字节时，
 * 装载必须拒绝且不创建实例（原实现门与装载分别读取）。 */
ZTEST(framework_wasm, test_f3_load_rejects_bytes_changed_after_gate)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0xAA;
	entelea_result_t rc;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	trial_confirm_slot0("", 1000);
	tp_mount(staged_len < 512U ? staged_len : 512U, false);
	rc = entelea_wasm_load(0, ctx, &app);
	tp_unmount();
	zassert_equal(rc, ENTELEA_E_PERM, "门后字节变化须拒装载");
	zassert_equal(app, 0xAA);
	zassert_equal(entelea_wasm_instance_count(), 0);
	zassert_equal(tp.hits, 2U, "两趟读取均发生（门摘要 + 捕获）");
}

/* F3：捕获趟读取失败原样为 E_IO，不创建实例。 */
ZTEST(framework_wasm, test_f3_capture_read_failure_propagates)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0xAA;
	entelea_result_t rc;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	trial_confirm_slot0("", 1000);
	tp_mount(staged_len < 512U ? staged_len : 512U, true);
	rc = entelea_wasm_load(0, ctx, &app);
	tp_unmount();
	zassert_equal(rc, ENTELEA_E_IO, "捕获趟读错误原样传播");
	zassert_equal(app, 0xAA);
	zassert_equal(entelea_wasm_instance_count(), 0);
	zassert_equal(tp.hits, 2U);
}

/* RF1 #9（G4）：执行闸——试用候选须经用户确认，且执行字节=确认对象。
 * 未确认（PENDING）候选不得装载（原 load 不查试用态：推送即可执行）。 */
ZTEST(framework_wasm, test_rf1_trial_pending_not_loadable)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0xAA;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(trial_submit(0, "", 1000), ENTELEA_OK);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_PERM,
		      "未确认候选不得装载");
	zassert_equal(app, 0xAA);
	zassert_equal(entelea_wasm_instance_count(), 0);
}

/* 确认后同槽重暂存他字节：确认不得随槽沿用（原无内容绑定）。 */
ZTEST(framework_wasm, test_rf1_trial_confirm_bound_to_bytes)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	trial_confirm_slot0("", 1000);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK,
		      "已确认字节可装载");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_PERM,
		      "同槽换字节不得沿用确认");
}

/* 门摘要域 = 整包文件 sha256 [0, pkg_len)（= Forge package_digest 域）：
 * 与测试侧独立计算的暂存字节摘要对拍；摘要读失败原样 E_IO、零副作用。 */
ZTEST(framework_wasm, test_persisted_pending_is_gated_after_restart)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0xAA;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(trial_submit(0, "", 1000), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_PERM,
		      "the real load path must restore and enforce PENDING");
	zassert_equal(app, 0xAA);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
}

ZTEST(framework_wasm, test_rf1_trial_digest_domain)
{
	entelea_principal_t ctx = {0};
	entelea_tsap_view_t v;
	uint8_t dg[32];
	uint32_t app = 0xAA;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL), ENTELEA_OK);
	zassert_equal(v.pkg_len, staged_len, "规范包长=签名末尾");
	zassert_equal(entelea_pkg_file_digest(0, v.pkg_len, dg), ENTELEA_OK);
	zassert_mem_equal(dg, staged_dg, 32, "摘要域=整包文件");
	zassert_equal(entelea_pkg_file_digest(2, v.pkg_len, dg), ENTELEA_E_PARAM);
	zassert_equal(entelea_pkg_file_digest(0, 0, dg), ENTELEA_E_PARAM);
	zassert_equal(entelea_pkg_file_digest(0, v.pkg_len, NULL), ENTELEA_E_PARAM);

	zassert_equal(entelea_wasm_init(), ENTELEA_OK);
	rd_mount(false, ENTELEA_PART_SLOT_A, 0, v.pkg_len);
	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL), ENTELEA_OK);
	zassert_equal(rd.hits, 0, "验签不读整包区间（注入只命中摘要读）");
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_E_IO, "摘要读错误");
	zassert_equal(rd.hits, 1);
	rd_unmount();
	zassert_equal(app, 0xAA);
	zassert_equal(entelea_wasm_instance_count(), 0);
}

/* call 复核门：装载后试用结束、同槽新候选待确认 → 旧实例 call=E_PERM
 * （不执行、不销毁）；新候选确认后旧字节实例仍拒（摘要不符）。 */
ZTEST(framework_wasm, test_rf1_trial_call_rechecks_gate)
{
	entelea_principal_t ctx = {0};
	entelea_trial_candidate_t c;
	uint32_t app = 0;
	int32_t ret = -1;

	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	trial_confirm_slot0("", 1000);
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, 100, &ret),
		      ENTELEA_OK, "ACTIVE+摘要相符可调用");

	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE)); /* 同槽他字节 */
	zassert_equal(trial_submit(0, "", 1000), ENTELEA_OK);
	ret = -77;
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, 100, &ret),
		      ENTELEA_E_PERM, "同槽新候选待确认：旧实例拒");
	zassert_equal(ret, -77, "拒绝不执行");
	zassert_equal(entelea_wasm_instance_count(), 1, "拒绝不销毁");
	entelea_trial_current(&c);
	zassert_equal(entelea_trial_confirm(&c), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 0, 100, &ret),
		      ENTELEA_E_PERM, "新候选已确认：旧字节实例仍拒");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}

/* 非候选 slot 不受试用政策管辖（natives 仅经稳定记录授权，见稳定集用例）。 */
ZTEST(framework_wasm, test_rf1_trial_other_slot_ungoverned)
{
	entelea_principal_t ctx = {0};
	uint32_t app = 0;

	stage_signed(1, WASM_PROBE, sizeof(WASM_PROBE));
	zassert_equal(trial_submit(1, "", 1000), ENTELEA_OK, "候选在 slot 1");
	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	zassert_equal(entelea_wasm_load(0, ctx, &app), ENTELEA_OK,
		      "非候选槽不受试用门管辖");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}

/* D-E + A1：晋升后的稳定记录使该槽在非候选状态下获得 natives 授权；撤销即失效；
 * 换字节（摘要不符）即使记录存在也无授权。 */
ZTEST(framework_wasm, test_stable_record_grants_natives_only_for_its_bytes)
{
	entelea_principal_t owner = mk_principal(1, "gpio:read:0");
	entelea_trial_candidate_t shown;
	entelea_trial_grant_t g;
	uint32_t app = 0;
	int32_t ret = -77;

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	trial_confirm_slot0("gpio:read:0", 1000);
	entelea_trial_current(&shown);
	zassert_equal(entelea_trial_promote(&shown), ENTELEA_OK, "第二次确认晋升");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);

	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK,
		      "非候选纯计算可装载");
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, 1, "稳定记录授予 natives");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	zassert_equal(entelea_stable_revoke(0), ENTELEA_OK);
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, owner, "run", 0, 100, &ret),
		      ENTELEA_OK);
	zassert_equal(ret, ENTELEA_E_PERM, "撤销后 natives 拒");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);

	stage_signed(0, WASM_PROBE, sizeof(WASM_PROBE));
	trial_confirm_slot0("gpio:read:0", 1000);
	entelea_trial_current(&shown);
	zassert_equal(entelea_trial_promote(&shown), ENTELEA_OK);
	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE)); /* 他字节：摘要不符 */
	zassert_equal(entelea_stable_grant(0, staged_dg, &g), ENTELEA_E_PERM,
		      "换字节后稳定记录不授予");
	zassert_equal(entelea_stable_grant(0, shown.digest, &g), ENTELEA_OK,
		      "记录仍对原字节有效");
	zassert_equal(entelea_wasm_load(0, owner, &app), ENTELEA_OK,
		      "换字节仍可纯计算装载");
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
}
