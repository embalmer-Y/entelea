/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TEST_AV_BOOT_H_
#define TEST_AV_BOOT_H_

#include <ts/appmgr.h>
#include <ts/hal.h>

ts_res_t __real_ts_av_config_bind(uint16_t app_id, ts_av_fmt_t fmt, uint16_t w, uint16_t h);
ts_res_t __real_ts_appmgr_app_start(uint16_t app_id, const uint8_t *wasm, uint32_t len,
				    const char *caps);

#endif
