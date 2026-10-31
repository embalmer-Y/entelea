/* SPDX-License-Identifier: Apache-2.0 */
/* tweetnacl 固件内自测钩子（DEC-49 调试期）：与主机对拍同源数据
 * （~/project/logs/tnacl/{sm,pub}.bin 导出的字节数组）——分离"库在模块
 * 构建下是否正确"与"verify 管线构造是否正确"两个变量。 */
#include "tweetnacl.h"

static const unsigned char self_sm[] = {
#include "tnacl_self_sm.inc"
};
static const unsigned char self_pk[32] = {
#include "tnacl_self_pk.inc"
};

int ts_tnacl_selftest(void)
{
	unsigned char m[1024];
	unsigned long long mlen = 0;

	return crypto_sign_open(m, &mlen, self_sm, sizeof(self_sm), self_pk);
}
