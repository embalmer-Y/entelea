/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_RESULT_H
#define ENTELEA_RESULT_H

#include <stdint.h>

typedef int32_t entelea_result_t;

enum {
	ENTELEA_OK = 0,
	ENTELEA_E_PARAM = -1,
	ENTELEA_E_NOMEM = -2,
	ENTELEA_E_PERM = -3,
	ENTELEA_E_STATE = -4,
	ENTELEA_E_BUSY = -5,
	ENTELEA_E_TIMEOUT = -6,
	ENTELEA_E_NOTFOUND = -7,
	ENTELEA_E_IO = -8,
	ENTELEA_E_INVALID_SIG = -9,
	ENTELEA_E_ROLLBACK_LIMIT = -10,
	ENTELEA_E_UNINIT = -11,
	ENTELEA_E_RANGE = -12,
	ENTELEA_PENDING = -13
};

#endif
