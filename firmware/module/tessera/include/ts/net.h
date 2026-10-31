/* SPDX-License-Identifier: Apache-2.0 */
/* ts-net 公共 API（design/LLD-ts-net.md）：zenoh-pico 会话 / 命名空间构造 /
 * 发布队列 / 心跳监视（断链判定源，合同 3）。
 * 传输缝纪律：zenoh 副作用全部收敛在 ts_net_transport_t 实现内（真实现 =
 * zenoh-pico〔CONFIG_TS_NET_ZENOH〕；测试/重放注入桩）——对齐 ts-store
 * backend 先例，保证 linkmon/pubq/session 逻辑确定性可测（合同 9）。 */
#ifndef TS_NET_H__
#define TS_NET_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 会话状态（LLD §2）--------------------------------------------------- */

typedef enum {
	TS_NET_DOWN,
	TS_NET_CONNECTED,
} ts_net_state_t;

/* ---- 传输缝 --------------------------------------------------------------- */

/* 发布服务类（DEC-42 QoS 映射）：安全事件 = 阻塞式高优先级；遥测/心跳 = 丢弃式。
 * 语义由传输实现承接（zenoh：congestion_control + priority）；非投递保证。 */
typedef enum {
	TS_NET_QOS_BESTEFFORT = 0,
	TS_NET_QOS_SAFETY = 1,
} ts_net_qos_t;

typedef struct ts_net_transport {
	ts_res_t (*open)(void); /* 建链（locator 自 prov 或测试注入） */
	void (*close)(void);
	/* 发布一行（DOWN 期不应被调用；失败由调用方按 pubq 语义丢弃计数） */
	ts_res_t (*publish)(const char *key, const uint8_t *payload, uint32_t len,
			    ts_net_qos_t qos);
	bool (*is_up)(void);
} ts_net_transport_t;

/* ---- 命名空间构造（LLD §3；前缀 tessera/<node>/<cube>）-------------------- */

/* 前缀注入：session 初始化时取自 prov（V1 单立方体 node = cube，DEC-26）；
 * 测试可直接设置。id 为 NULL/空 → TS_E_PARAM；超长 → TS_E_PARAM。 */
ts_res_t ts_net_set_ids(const char *node_id, const char *cube_id);

/* 以下构造器返回 snprintf 语义（成功 = 写入字节数；n 不足 = 需要的字节数，
 * 不写越界；key 语法变更 = review 门 ③）。 */
int ts_net_key_cmd(char *buf, size_t n, const char *uid); /* …/<uid>/cmd      */
int ts_net_key_tel(char *buf, size_t n, const char *uid); /* …/<uid>/telemetry */
int ts_net_key_evt(char *buf, size_t n, const char *uid); /* …/<uid>/event    */
int ts_net_key_hb(char *buf, size_t n, bool host_dir);    /* …/sys/hb[-host]（DR-12） */
int ts_net_key_sys(char *buf, size_t n, const char *cmd); /* …/sys/<cmd>（DR-03） */
int ts_net_key_av(char *buf, size_t n, uint16_t app_id);  /* …/av/<id>/frame（DEC-47①） */

/* ---- 会话（LLD §2）------------------------------------------------------- */

ts_net_state_t ts_net_state(void);
void ts_net_set_transport(const ts_net_transport_t *t); /* NULL = 摘除 */
/* 退避表查值（DEC-27 固定表 250/500/1000/2000 循环；attempt 从 0 起，无随机） */
uint32_t ts_net_backoff_ms(uint32_t attempt);
/* 周期推进（net_wq / 测试）：DOWN → 按退避表尝试 open；CONNECTED → 掉线
 * 检测 + 迁移；成功建链后冲刷 pubq。返回本 tick 后状态。 */
ts_net_state_t ts_net_session_poll(uint64_t now_ms);
/* 媒体掉线提示（板级七增补，LLD §2）：承载静默掉线（WiFi 断联等）时 TCP
 * 半开、is_up 检测分钟级才收敛——glue 在承载事件上调用本接口，会话在下
 * 一 poll（net_wq 上下文）立即判 DOWN 并按退避表重连。任意上下文可调。 */
void ts_net_session_media_down(void);

/* ---- 发布队列（LLD §5：遥测尽力而为，不阻塞控制路径）---------------------- */

/* 入队（CONNECTED 时可直接发送）；DOWN 期 = 直接丢弃并计数（防上电风暴）；
 * 队满 = 丢最旧并计数。key/payload 超容量 → TS_E_PARAM。 */
ts_res_t ts_net_pubq_push(const char *key, const uint8_t *payload, uint32_t len);
/* 显式服务类入队（DEC-42）：安全事件走阻塞式高优先级；push = BESTEFFORT 缺省。 */
ts_res_t ts_net_pubq_push_qos(const char *key, const uint8_t *payload, uint32_t len,
			      ts_net_qos_t qos);
uint32_t ts_net_pubq_dropped(void); /* 累计丢弃（含 DOWN 期丢与溢出丢） */
void ts_net_pubq_flush(void);       /* 经 transport 逐条发送（发送后清出） */

/* ---- av 分片通道（DEC-47①②，MD1.2g；LLD §5）------------------------------
 * APP 大块数据发布面（视频帧分片）：APP 线程入队（信封封装 fid/cid/n/crc/d，
 * 满队 = TS_E_BUSY 背压）；net 线程周期冲刷（打拍 ≥TS_NET_PUBLISH_MIN_GAP_MS
 * + 分片重试 ≤RETRY_MAX×RETRY_DELAY_MS——DEC-47② 实测值）。DOWN 期丢弃 +
 * 计数（尽力而为；消费端按 crc/帧序丢弃残帧）。 */

ts_res_t ts_net_avq_push(uint16_t app_id, uint32_t fid, uint32_t cid,
			 uint32_t n_chunks, const uint8_t *data, uint32_t len);
void ts_net_avq_flush(void);
uint32_t ts_net_avq_dropped(void);
void ts_net_avq_reset(void); /* 测试复位 */

/* ---- 心跳监视（LLD §6，合同 3 判定源；合同 8 本地独立判定）---------------- */

/* 周期调用（sysworkq / 测试；now_ms 显式传入——虚拟时钟确定性）：
 * 发送 cube→host 心跳（transport 在位且 UP 时）+ host 心跳超时判定。 */
void ts_net_linkmon_tick(uint64_t now_ms);
/* host 心跳到达（订阅回调 / 测试注入）：更新时间戳与恢复连胜计数。 */
void ts_net_linkmon_hb_host(uint64_t now_ms);
bool ts_net_link_up(void); /* 当前判定（喂 ts_safety_set_link 的同一状态） */
uint32_t ts_net_linkmon_hb_seq(void); /* 已发心跳序号（payload 内容源） */

/* ---- 命令面（LLD §4；sys 命令表 host_only——DEC-30①/DR-03）--------------- */

/* 命令处理器：args 为解码后的固定参数集（V1 sys 面）；resp 写入回执 data 段
 * （CBOR 编码，写入字节数经 *resp_len 返回）。返回值 = 回执 status。 */
typedef struct {
	char confirm[16]; /* estop-clear 确认令牌 */
	bool has_time_ms;
	uint64_t time_ms; /* set-time 墙钟数据字段（DR-08） */
	bool has_holder;
	char holder[24];  /* 控制租约持有者标识（DEC-41） */
	bool has_total;
	uint32_t total;    /* app-begin：包总长（LLD-agent-deploy-net §3） */
	bool has_app;
	uint8_t app;       /* V0.4.0 M7b（Q-32⑤）：v2 信封 app 字段（app_id，
			    * 1..N；缺省 = 1——旧客户端零改动兼容） */
	bool has_offset;
	uint32_t offset;   /* app-chunk：槽内偏移 */
	bool has_src_flash; /* get-audit audit_src="flash"（DEC-52①：flash 环读取） */
	/* app-chunk 数据（零拷贝引用——指向请求缓冲内部，生存期 = handler 调用域；
	 * 长度上限 CONFIG_TS_NET_APP_CHUNK_MAX） */
	const uint8_t *chunk;
	uint32_t chunk_len;
	/* CG5-B（Q-39③ 方案 A）：宿主资源推送面 fs-begin/chunk/commit */
	bool has_path;
	char path[68];    /* fs-begin：设备侧目标绝对路径（fs_paths 同文法上限） */
	bool has_sha256;
	uint8_t sha256[32]; /* fs-begin：整文件摘要（fs-commit 读回校验对象） */
} ts_net_cmd_args_t;

typedef ts_res_t (*ts_net_cmd_fn)(const ts_net_cmd_args_t *args,
				  uint8_t *resp, size_t cap, size_t *resp_len);

/* 注册命令（key 后缀如 "sys/get-info"；容量 12，满 = TS_E_NOMEM——
 * 7 项基础 sys + 3 项租约（DEC-41）+ 余量）。V1 仅框架 init 期注册（sys 面
 * host_only）；APP 侧注册随 msg 类（M2b.2）。 */
ts_res_t ts_net_cmd_register(const char *suffix, ts_net_cmd_fn fn);

/* 分发一条命令（zenoh query 回调 / 测试直调）。请求两种形态（DEC-40）：
 *   v1：map{"op", "args"?}（弃用期，回执 = map{"status", "data"}）；
 *   v2：map{"ver":1,"kind":1,"rid","src","op","args"?{"idem"?,"to"?}}，
 *        回执 = map{"ver":1,"kind":16,"rid"<回带>,"status","data"}。
 * 按首键判别（"op"=v1 / "ver"=v2）；未知键/超集 = TS_E_PARAM 回执（fail-closed）。
 * idem 命中 = 回放缓存回执不重执行（DEC-40，深度 CONFIG_TS_NET_IDEM_CACHE LRU）；
 * to > 5000ms = TS_E_PARAM（断链窗口 6000ms − 余量，DEC-40）。 */
ts_res_t ts_net_cmd_dispatch(const char *key_suffix, const uint8_t *req, uint32_t req_len,
			     uint8_t *resp, size_t cap, size_t *resp_len);

/* ---- 发布面（LLD §5：遥测快照 + 事件外发）------------------------------- */

/* 事件订阅注册（SAFE_STATE_CHANGED/PERM_DENIED/INPUT_CHANGED/ESTOP → pubq；
 * idempotent，容量受事件总线 MAX_SUBS 限制）。 */
ts_res_t ts_net_pub_init(void);

/* 周期遥测快照（init 周期驱动/测试直调）：遍历输出实例 → readback →
 * 各自 telemetry key 入 pubq（经 pubq 语义尽力而为）。 */
void ts_net_pub_telem(uint64_t now_ms);

/* ---- 初始化（boot 步骤接线；LLD §2/§6）---------------------------------- */

/* ids ← prov（V1 单立方体 node = cube，DEC-26；prov 缺失/损坏 → 开发缺省
 * "n-dev"/"c-dev" 且网络面保持 DOWN——缺 prov 的安全效应 = 无链路 = 安全侧；
 * 生产板 boot 期强校验随板级里程碑收紧）。CONFIG_TS_NET_ZENOH 时绑定真实
 * 传输并启动 sysworkq 周期驱动（session_poll + linkmon_tick + pub_telem）。 */
ts_res_t ts_net_init(void);

/* ---- 测试钩子（仅 CONFIG_TS_TEST）---------------------------------------- */
#ifdef CONFIG_TS_TEST
/* 全量复位（session/pubq/linkmon 状态 + 摘除传输注入）；不触碰 safety 注册表 */
void ts_net_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TS_NET_H__ */
