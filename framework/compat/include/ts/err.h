/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TS_ERR_H__
#define TS_ERR_H__

#include <entelea/result.h>

typedef entelea_result_t ts_res_t;
#define TS_OK 0
#define TS_E_PARAM (-1)
#define TS_E_NOMEM (-2)
#define TS_E_PERM (-3)
#define TS_E_STATE (-4)
#define TS_E_BUSY (-5)
#define TS_E_TIMEOUT (-6)
#define TS_E_NOTFOUND (-7)
#define TS_E_IO (-8)
#define TS_E_INVALID_SIG (-9)
#define TS_E_ROLLBACK_LIMIT (-10)
#define TS_E_UNINIT (-11)
#define TS_E_RANGE (-12)
#define TS_E_PENDING (-13)

#define TS_FAIL_SRC_BOOT 0x0001U
#define TS_FAIL_SRC_WDT 0x0002U
#define TS_FAIL_SRC_SAFETY 0x0003U
#define TS_FAIL_SRC_STORE 0x0004U
#define TS_FAIL_BOOT(idx) ((uint32_t)TS_FAIL_SRC_BOOT << 16 | (uint32_t)(idx))
#define TS_FAIL_WDT(src) ((uint32_t)TS_FAIL_SRC_WDT << 16 | (uint32_t)(src))

struct ts_ctx_opaque;
typedef struct ts_ctx_opaque ts_ctx_t;

#endif
