/* SPDX-License-Identifier: Apache-2.0 */
/* 命名空间构造（LLD-ts-net §3）。前缀 tessera/<node>/<cube>（node 段 =
 * DEC-02 预留层；单立方体时 node = cube，DEC-26）。key 语法变更 = 门 ③。 */
#include <stdio.h>
#include <string.h>
#include <ts/net.h>
#include "internal.h"

char ts_net_prefix[48];

ts_res_t ts_net_set_ids(const char *node_id, const char *cube_id)
{
	if (node_id == NULL || cube_id == NULL || node_id[0] == '\0' ||
	    cube_id[0] == '\0') {
		return TS_E_PARAM;
	}
	/* 原子拒绝：先在临时缓冲成型，成功才落位——失败不触碰现有前缀 */
	char tmp[sizeof(ts_net_prefix)];
	int w = snprintf(tmp, sizeof(tmp), "tessera/%s/%s", node_id, cube_id);

	if (w < 0 || (size_t)w >= sizeof(tmp)) {
		return TS_E_PARAM;
	}
	memcpy(ts_net_prefix, tmp, (size_t)w + 1);
	return TS_OK;
}

/* 共用拼接：prefix + 后缀段（返回 snprintf 语义，不越界写） */
static int key_cat(char *buf, size_t n, const char *suffix)
{
	return snprintf(buf, n, "%s/%s", ts_net_prefix, suffix);
}

int ts_net_key_cmd(char *buf, size_t n, const char *uid)
{
	if (uid == NULL || uid[0] == '\0') return -1;
	char suf[40];
	int w = snprintf(suf, sizeof(suf), "%s/cmd", uid);
	if (w < 0 || (size_t)w >= sizeof(suf)) return -1;
	return key_cat(buf, n, suf);
}

int ts_net_key_tel(char *buf, size_t n, const char *uid)
{
	if (uid == NULL || uid[0] == '\0') return -1;
	char suf[40];
	int w = snprintf(suf, sizeof(suf), "%s/telemetry", uid);
	if (w < 0 || (size_t)w >= sizeof(suf)) return -1;
	return key_cat(buf, n, suf);
}

int ts_net_key_evt(char *buf, size_t n, const char *uid)
{
	if (uid == NULL || uid[0] == '\0') return -1;
	char suf[40];
	int w = snprintf(suf, sizeof(suf), "%s/event", uid);
	if (w < 0 || (size_t)w >= sizeof(suf)) return -1;
	return key_cat(buf, n, suf);
}

int ts_net_key_hb(char *buf, size_t n, bool host_dir)
{
	/* DR-12：cube→host = …/sys/hb；host→cube = …/sys/hb-host */
	return key_cat(buf, n, host_dir ? "sys/hb-host" : "sys/hb");
}

int ts_net_key_sys(char *buf, size_t n, const char *cmd)
{
	if (cmd == NULL || cmd[0] == '\0') return -1;
	char suf[32];
	int w = snprintf(suf, sizeof(suf), "sys/%s", cmd);
	if (w < 0 || (size_t)w >= sizeof(suf)) return -1;
	return key_cat(buf, n, suf);
}

int ts_net_key_av(char *buf, size_t n, uint16_t app_id)
{
	/* DEC-47①（MD1.2g）：专用帧分片通道 …/av/<stream>/frame；
	 * V1 stream = 数值 app_id（单活跃 = 1——避免字符串段注入面） */
	char suf[24];
	int w = snprintf(suf, sizeof(suf), "av/%u/frame", (unsigned)app_id);

	if (w < 0 || (size_t)w >= sizeof(suf)) return -1;
	return key_cat(buf, n, suf);
}
