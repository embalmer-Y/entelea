/* SPDX-License-Identifier: Apache-2.0 */
/* M0 冒烟测试：验证 tessera 模块接线（DoD：模块 Kconfig 被构建吸收）。 */
#include <zephyr/ztest.h>
#include <zephyr/sys/util.h>

ZTEST(smoke, test_module_wired)
{
	/* 来源: design/HLD-firmware-framework.md §7 M0 */
	zassert_true(IS_ENABLED(CONFIG_TS_MODULE),
		     "tessera module not wired into build");
}

ZTEST_SUITE(smoke, NULL, NULL, NULL, NULL, NULL);
