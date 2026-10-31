/* SPDX-License-Identifier: Apache-2.0 */
/* ts-net 内部共享（不对外）。 */
#ifndef TS_NET_INTERNAL_H__
#define TS_NET_INTERNAL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/net.h>

/* keyspace.c 定义，session.c/linkmon.c/pubq.c 共享 */
extern char ts_net_prefix[48]; /* "tessera/<node>/<cube>"，构造期一次成型 */

/* session.c 定义：当前传输（NULL = 未注入） */
extern const ts_net_transport_t *ts_net_transport;

/* cmd.c：sys 命令表注册（init 期一次；host_only——DEC-30①）。
 * 返回 TS_E_NOMEM/PARAM = 装配错误（表满/撞名），init 上抛 → boot fail-safe。 */
ts_res_t ts_net_cmd_sys_init(void);
#ifdef CONFIG_TS_TEST
void ts_net_cmd_test_reset(void); /* 表 + 幂等缓存清空（测试隔离） */
#endif

/* ---- lease.c：控制租约（DEC-41）--------------------------------------------
 * V1 单租约；TTL = CONFIG_TS_NET_LEASE_TTL_MS（默认 10s）；惰性过期（访问时
 * 判定，无定时器——确定性）；只管命令准入，不联动安全态（单源纪律）。 */
#define TS_NET_LEASE_HOLDER_MAX 24 /* 与 ts_net_cmd_args_t.holder 同宽 */
/* 获取/续期：无有效租约 → 授予新 id；同持有者 → 续期（幂等，id 不变）；
 * 他人持有 → TS_E_STATE（*id 与 *expires_at_ms 回填当前租约供拒绝回执）。
 * now_ms 显式传入（虚拟时钟确定性）；lease_id 从 1 起单调递增不复用。 */
ts_res_t ts_net_lease_acquire(const char *holder, uint64_t now_ms,
			      uint32_t *id, uint64_t *expires_at_ms);
/* 归还（幂等）：无有效租约 = TS_OK；他人持有 = TS_E_STATE。 */
ts_res_t ts_net_lease_release(const char *holder, uint64_t now_ms);
/* 快照（出参可 NULL）：valid = 有效租约在册且未过期。 */
void ts_net_lease_get(uint64_t now_ms, bool *valid, char *holder, size_t holder_cap,
		      uint32_t *id, uint64_t *expires_at_ms);
/* 准入判定（M2b.2 写命令面挂钩点；V1 sys 面只读族与 estop-clear 豁免）。 */
bool ts_net_lease_held_by(const char *holder, uint64_t now_ms);
#ifdef CONFIG_TS_TEST
void ts_net_lease_test_reset(void); /* 租约状态 + id 计数器清零（测试隔离） */
#endif

/* ---- cbor_min：确定性子集编解码（cmd 请求/回执 + pub payload）-------------
 * 只支持 definite map/tstr/uint/negint/bool/array；任何超集 = 解码失败。
 * 编码为 canonical 序（由调用方按固定键序写入）。 */

typedef struct {
	const uint8_t *p;
	size_t rem;
} ts_cbor_rd_t;

void ts_cbor_rd_init(ts_cbor_rd_t *r, const uint8_t *buf, size_t len);
/* 逐项读取：失败返回 false（整个请求即非法，fail-closed） */
bool ts_cbor_map_open(ts_cbor_rd_t *r, uint32_t *pairs);
bool ts_cbor_array_open(ts_cbor_rd_t *r, uint32_t *items);
bool ts_cbor_tstr(ts_cbor_rd_t *r, char *out, size_t cap);
bool ts_cbor_bstr_ref(ts_cbor_rd_t *r, const uint8_t **out, uint32_t *len);
bool ts_cbor_uint(ts_cbor_rd_t *r, uint64_t *out);
bool ts_cbor_int(ts_cbor_rd_t *r, int64_t *out);
bool ts_cbor_bool(ts_cbor_rd_t *r, bool *out);
bool ts_cbor_skip(ts_cbor_rd_t *r, int depth); /* CG4：scan 类走查跳过非目标键 */

/* 编码器：buf 追加写（*pos 前进）；溢出由调用方检查（resp 定容） */
bool ts_cbor_put_map(uint8_t *buf, size_t cap, size_t *pos, uint32_t pairs);
bool ts_cbor_put_array(uint8_t *buf, size_t cap, size_t *pos, uint32_t items);
bool ts_cbor_put_tstr(uint8_t *buf, size_t cap, size_t *pos, const char *s);
bool ts_cbor_put_tstrn(uint8_t *buf, size_t cap, size_t *pos, const char *s, size_t n);
bool ts_cbor_put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *b, uint32_t len);
bool ts_cbor_put_uint(uint8_t *buf, size_t cap, size_t *pos, uint64_t v);
bool ts_cbor_put_int(uint8_t *buf, size_t cap, size_t *pos, int64_t v);
bool ts_cbor_put_bool(uint8_t *buf, size_t cap, size_t *pos, bool v);

#endif /* TS_NET_INTERNAL_H__ */
