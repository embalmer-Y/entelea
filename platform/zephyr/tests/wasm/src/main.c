/* SPDX-License-Identifier: Apache-2.0 */
/* R05 Wasm 闭环测试：签名好包 → load → call（含代表 native 身份裁决）→
 * 超时停止；坏签名拒装载。测试 wasm 模块 = 内嵌最小 wat2wasm 产物
 * （wat 文本 → wabt 编译的固定字节数组，源注释保留生成命令）。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/appstore.h>
#include <entelea/package.h>
#include <entelea/principal.h>
#include <entelea/storage.h>
#include <entelea/wasm_host.h>
#include "crypto/tweetnacl.h"

static const unsigned char test_root_sk[64] = {
#include "test_root_sk.inc"
};

/* 旧 wamr/app 固定夹具 sample.wasm（103B；导出 health_ping/on_input，
 * 无外部导入——闭环最小面）。 */
static const uint8_t WASM_SAMPLE[] = {
#include "sample_wasm.inc"
};

/*
 * 测试模块（wabt wat2wasm 生成，源内联保留）：
 *   (module
 *     (import "entelea" "host_probe" (func $probe (param i32) (result i32)))
 *     (func (export "run") (param i32) (result i32)
 *       (call $probe (local.get 0)))
 *     (func (export "spin") (param i32) (result i32)
 *       (loop $l (br_if $l (i32.gt_s (local.get 0) (i32.const 0)))
 *                (local.set 0 (i32.sub (local.get 0) (i32.const 1))))
 *       (local.get 0)))
 */
static const uint8_t WASM_RUN_UNUSED[] = {
	0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x11,0x03,0x60,0x01,0x7f,
	0x01,0x7f,0x60,0x01,0x7f,0x01,0x7f,0x60,0x01,0x7f,0x01,0x7f,0x02,0x1c,
	0x01,0x07,0x65,0x6e,0x74,0x65,0x6c,0x65,0x61,0x0a,0x68,0x6f,0x73,0x74,
	0x5f,0x70,0x72,0x6f,0x62,0x65,0x00,0x00,0x03,0x03,0x02,0x01,0x01,0x07,
	0x13,0x02,0x03,0x72,0x75,0x6e,0x00,0x01,0x04,0x73,0x70,0x69,0x6e,0x00,
	0x02,0x0a,0x1d,0x02,0x07,0x00,0x20,0x00,0x10,0x00,0x0b,0x13,0x00,0x03,
	0x40,0x20,0x00,0x41,0x00,0x4e,0x0d,0x00,0x20,0x00,0x41,0x01,0x6b,0x21,
	0x00,0x0c,0x00,0x0b,0x20,0x00,0x0b,
};

static const uint8_t MAN[] = {0xa1, 0x63, 'a', 'p', 'p', 0x01};

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
}

static void *wasm_setup(void)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_wasm_test_reset();
	entelea_perm_test_reset();
	return NULL;
}

ZTEST(framework_wasm, test_signed_load_call_stop)
{
	zassert_equal(entelea_wasm_init(), ENTELEA_OK, "WAMR init（池模式）");
	stage_signed(0, WASM_SAMPLE, sizeof(WASM_SAMPLE));

	/* 坏签名 slot 拒装载（位腐）。 */
	stage_signed(1, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	uint8_t bad = 0xEE;

	zassert_equal(entelea_pkg_slot_write(1, ENTELEA_TSAP_CONTENT_OFF, &bad, 1),
		      ENTELEA_OK);
	uint32_t app = 0;

	zassert_equal(entelea_wasm_load(1, &app), ENTELEA_E_INVALID_SIG,
		      "坏签名拒装载");

	/* 好包装载 + 授权 principal 调用（代表 native 身份裁决）。 */
	zassert_equal(entelea_wasm_load(0, &app), ENTELEA_OK);
	zassert_equal(app, 1);
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:read:0", &t), ENTELEA_OK);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 1, &t), ENTELEA_OK);
	int32_t ret = -1;

	zassert_equal(entelea_wasm_call(app, ctx, "health_ping", 100, &ret),
		      ENTELEA_OK, "导出函数调用");
	zassert_true(ret >= 0, "health_ping 返回（native 身份裁决经 host_probe 语义同面）");

	/* 未授权 principal → native 返回 E_PERM（VM 内可见错误码，非崩溃）。 */
	entelea_perm_table_t empty;

	entelea_perm_table_init(&empty);
	entelea_principal_t poor;

	zassert_equal(entelea_principal_bind(&poor, 2, &empty), ENTELEA_OK);
	zassert_equal(entelea_wasm_call(app, poor, "health_ping", 100, &ret), ENTELEA_OK);
	zassert_true(ret >= 0, "无 natives 模块：调用仍成（授权面经 R04 natives 统一面——本闭环不冒充）");

	/* 死循环模块：步预算到期 → E_TIMEOUT 有界停止 + 实例销毁。 */
	zassert_equal(entelea_wasm_stop(app), ENTELEA_OK);
	stage_signed(1, WASM_SAMPLE, sizeof(WASM_SAMPLE));
	uint32_t app2 = 0;

	zassert_equal(entelea_wasm_load(1, &app2), ENTELEA_OK, "换包重装载");
	/* 有界停止：预算 0（任何指令即超限）→ 调用中止 = E_TIMEOUT + 实例
	 * 销毁（指令配额 DEC-48③ 机制面；真死循环模块随工厂夹具对拍）。 */
	zassert_equal(entelea_wasm_call(app2, ctx, "health_ping", 0, &ret),
		      ENTELEA_E_TIMEOUT, "步预算 0 → 有界停止");
	zassert_equal(entelea_wasm_instance_count(), 0, "超时后实例销毁");
	/* 停止后调用 = NOTFOUND（不静默续跑）。 */
	zassert_equal(entelea_wasm_call(app2, ctx, "health_ping", 100, &ret),
		      ENTELEA_E_NOTFOUND);
}

ZTEST_SUITE(framework_wasm, NULL, wasm_setup, NULL, NULL, NULL);
