#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: sh run-netmon-concurrency.sh <output-dir>" >&2
	exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
OUT=$1
CC=${CC:-cc}

case "${SANITIZE:-}" in
	"") set -- ;;
	thread) set -- -fsanitize=thread -fno-omit-frame-pointer ;;
	*) echo "FAIL: unsupported SANITIZE value" >&2; exit 2 ;;
esac

mkdir -p "$OUT"
"$CC" -D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -pthread "$@" \
	-Iplatform/linux/zephyr_inc -Iplatform/linux -Iplatform/zephyr \
	-Iframework/include \
	framework/services/net/netmon.c platform/linux/tests/netmon_concurrency.c \
	-Wl,--gc-sections -o "$OUT/netmon-concurrency"
"$OUT/netmon-concurrency"
