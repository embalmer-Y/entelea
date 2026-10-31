/* SPDX-License-Identifier: Apache-2.0 */
/* 插拔事件（LLD-ts-periph §3，V1 范围）。
 * DETACH 语义：通道进 SAFE_FAULT（物理不在场 = 故障态，输出拒绝）+
 * key 下线通告（事件外发）；ATTACH = 上电态重放 + 事件。
 * 真机检测（连接器感知）= 第三阶段硬件定型后补；V1 触发源 = 测试/native_sim 桩。 */
#include <string.h>
#include <ts/core.h>
#include <ts/periph.h>
#include <ts/safety.h>
#include <zephyr/kernel.h>
#include "internal.h"

const ts_periph_desc_t *periph_find(const char *uid)
{
	if (uid == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < ts_periph_count(); i++) {
		const ts_periph_desc_t *d = ts_periph_get(i);

		if (d != NULL && strcmp(d->uid, uid) == 0) {
			return d;
		}
	}
	return NULL;
}

static void publish_periph_evt(uint16_t id, const ts_periph_desc_t *d)
{
	const ts_periph_evt_t pl = {.uid = d->uid, .kind = (uint8_t)d->kind};
	const ts_evt_t evt = {
		.id = (ts_evt_id_t)id, .t_ms = ts_time_ms(),
		.data = &pl, .len = sizeof(pl),
	};
	ts_evt_publish(&evt);
}

ts_res_t ts_periph_detach(const char *uid)
{
	const ts_periph_desc_t *d = periph_find(uid);

	if (d == NULL) {
		return TS_E_NOTFOUND;
	}
	if (d->kind != TS_PK_ADC) {
		ts_res_t r = ts_safety_force_channel_fault(uid);

		if (r != TS_OK) {
			return r;
		}
	}
	publish_periph_evt(TS_EVT_PERIPH_DETACH, d);
	return TS_OK;
}

ts_res_t ts_periph_attach(const char *uid)
{
	const ts_periph_desc_t *d = periph_find(uid);

	if (d == NULL) {
		return TS_E_NOTFOUND;
	}
	if (d->kind != TS_PK_ADC) {
		ts_res_t r = ts_safety_channel_recover(uid);

		if (r != TS_OK) {
			return r;
		}
	}
	publish_periph_evt(TS_EVT_PERIPH_ATTACH, d);
	return TS_OK;
}
