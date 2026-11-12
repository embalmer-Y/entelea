#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -ne 4 ]; then
	echo "usage: sh run-cloud-diagnostic.sh <mbedtls-source> <psa-source> <mbedtls-build> <output-dir>" >&2
	exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
MBEDTLS=$1
PSA=$2
BUILD=$3
OUT=$4
CC=${CC:-cc}

python3 tools/check_dependencies.py --dependency mbedtls --checkout "$MBEDTLS"
python3 tools/check_dependencies.py --dependency tf_psa_crypto --checkout "$PSA"
mkdir -p "$OUT"
"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
	-Iframework/include -Iplatform/linux \
	-I"$MBEDTLS/include" -I"$PSA/include" \
	-I"$BUILD/include" -I"$BUILD/tf-psa-crypto/include" \
	-I"$PSA/drivers/builtin/include" -I"$PSA/core/include" \
	-I"$PSA/dispatch/include" \
	platform/linux/tests/cloud_diagnostic.c -o "$OUT/cloud-diagnostic"
"$OUT/cloud-diagnostic"
