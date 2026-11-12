/* SPDX-License-Identifier: Apache-2.0 */
/* Firmware self-test hook for tweetnacl (DEC-49 debug phase). It uses the
 * same data as the host cross-check (byte arrays exported from
 * ~/project/logs/tnacl/{sm,pub}.bin), separating two variables: whether
 * the library is correct when built as a module, and whether the verify
 * pipeline is constructed correctly. */
#include "tweetnacl.h"

static const unsigned char self_sm[] = {
#include "tnacl_self_sm.inc"
};
static const unsigned char self_pk[32] = {
#include "tnacl_self_pk.inc"
};

int
ts_tnacl_selftest(void)
{
	unsigned char m[1024];
	unsigned long long mlen = 0;

	return crypto_sign_open(m, &mlen, self_sm, sizeof(self_sm), self_pk);
}
