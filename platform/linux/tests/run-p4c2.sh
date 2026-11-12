#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: sh run-p4c2.sh <output-dir>" >&2
	exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
OUT=$1
CC=${CC:-cc}

case "${SANITIZE:-}" in
	"") set -- ;;
	address) set -- -fsanitize=address,undefined -fno-omit-frame-pointer ;;
	*) echo "FAIL: unsupported SANITIZE value" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
"$CC" -std=c11 -Wall -Wextra -Werror "$@" \
	platform/linux/tests/p4c2.c -o "$OUT/p4c2"
"$OUT/p4c2"
