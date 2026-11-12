#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -ne 4 ]; then
	echo "usage: sh run.sh <mbedtls-source> <psa-source> <mbedtls-build> <output-dir>" >&2
	exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
cd "$ROOT"
MBEDTLS=$1
PSA=$2
BUILD=$3
OUT=$4
CC=${CC:-cc}

python3 tools/check_dependencies.py --dependency mbedtls --checkout "$MBEDTLS"
python3 tools/check_dependencies.py --dependency tf_psa_crypto --checkout "$PSA"
mbed_pin=$(git -C "$MBEDTLS" rev-parse HEAD)
psa_pin=$(git -C "$PSA" rev-parse HEAD)
if [ ! -f "$BUILD/build-stamp" ] ||
   [ "$(cat "$BUILD/build-stamp")" != "$mbed_pin/$psa_pin" ]; then
	echo "FAIL: missing or mismatched TLS build stamp; no dependency build attempted" >&2
	exit 1
fi
for lib in libmbedtls.a libmbedx509.a libmbedcrypto.a; do
	if [ ! -f "$BUILD/library/$lib" ]; then
		echo "FAIL: missing $BUILD/library/$lib" >&2
		exit 1
	fi
done
mkdir -p "$OUT"

compile_probe() {
	name=$1
	source=$2
	platform=$3
	shift 3
	"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
	  -Iplatform/zephyr/tests/https_host/include -Iframework/include \
	  -I"$platform" -I"$MBEDTLS/include" -I"$PSA/include" \
	  -I"$BUILD/include" -I"$BUILD/tf-psa-crypto/include" \
	  -I"$PSA/drivers/builtin/include" -I"$PSA/core/include" \
	  -I"$PSA/dispatch/include" "$@" \
	  platform/zephyr/tests/https_host/src/main.c "$source" -o "$OUT/$name" \
	  -Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo -Wl,--wrap=socket \
	  -Wl,--wrap=connect -Wl,--wrap=setsockopt -Wl,--wrap=send \
	  -Wl,--wrap=recv -Wl,--wrap=close -Wl,--wrap=mbedtls_ssl_set_bio \
	  -Wl,--wrap=mbedtls_ssl_handshake -Wl,--wrap=mbedtls_ssl_write \
	  -Wl,--wrap=mbedtls_ssl_read -Wl,--wrap=mbedtls_ssl_free \
	  "$BUILD/library/libmbedtls.a" "$BUILD/library/libmbedx509.a" \
	  "$BUILD/library/libmbedcrypto.a" -lpthread
}

compile_probe zephyr-https platform/zephyr/https_chat.c platform/zephyr
compile_probe linux-https platform/linux/https_chat.c platform/linux \
  -DENTELEA_HTTPS_HOST_LINUX=1 -Wl,--wrap=clock_gettime \
  -Wl,--wrap=mbedtls_net_connect -Wl,--wrap=mbedtls_net_free \
  -Wl,--wrap=mbedtls_x509_crt_parse_file

for target in zephyr-https linux-https; do
	for mode in stall drip dns connect setsockopt write read stale_errno \
	  success unicode_input escaped truncated invalid_escape missing_close \
	  short_unicode unpaired_surrogate invalid_response_utf8 escaped_nul \
	  bad_json_number duplicate_content \
	  wrong_content chunked chunked_truncated length_short header_decoy \
	  leading_space bad_header invalid_utf8 overflow connect_delay dns_delay \
	  handshake_late eof_late clock_fail clock_rollback recv_eagain send_eagain \
	  secret_wipe_success secret_wipe_failure \
	  bad_response_name bad_response_value bad_trailer unclean_eof \
	  unclean_length unclean_chunked raw_eof; do
		timeout 5 "$OUT/$target" "$mode"
	done
	printf 'PASS: %s fault-injection cases\n' "$target"
done
