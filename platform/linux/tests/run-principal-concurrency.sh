#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "usage: sh run-principal-concurrency.sh <output-dir> [msg|native-guess]" >&2
	exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
OUT=$1
shift
CC=${CC:-cc}

mkdir -p "$OUT"
"$CC" -D_POSIX_C_SOURCE=200809L -DENTELEA_TEST=1 -std=c11 \
	-Wall -Wextra -Werror -pthread \
	-Iplatform/linux/zephyr_inc -Iplatform/linux -Iframework/include \
	framework/execution/capability/perm.c \
	platform/linux/tests/principal_concurrency.c \
	-o "$OUT/principal-concurrency"
"$OUT/principal-concurrency" "$@"
