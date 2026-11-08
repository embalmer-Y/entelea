#!/bin/sh
# R07 Linux 最小宿主构建（D02）：同源 framework 源 + POSIX 垫片。
set -e
cd "$(dirname "$0")/.."
OUT=build/r07-linux-host
mkdir -p "$OUT"
CC=${CC:-cc}
SRC="platform/linux/host_main.c platform/linux/clock.c platform/linux/zephyr_shim.c \
 framework/common/time.c framework/runtime/supervision/events.c \
 framework/runtime/supervision/wdt.c framework/services/storage/audit.c \
 framework/services/storage/part.c \
 framework/execution/safety/channel.c framework/execution/safety/commit.c \
 framework/execution/safety/force.c framework/execution/safety/drivers.c"
INC="-Iframework/include -Iplatform/linux -Iplatform/linux/zephyr_inc"
DEF="-DENTELEA_TEST=1 -DZEPHYR_KERNEL_SHIM=1"
# events.c/safety 源 #include <zephyr/kernel.h>/<zephyr/...> —— 以 shim 头覆盖。
$CC -std=c11 -Wall -Wextra -Werror $INC $DEF -include platform/linux/zephyr_shim.h \
  $SRC -o "$OUT/host" -lpthread 2>&1 | head -20
echo "[linux-host] built $OUT/host"
