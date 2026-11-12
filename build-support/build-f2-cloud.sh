#!/bin/sh
# F-2 真云闸（Linux host）：mbedtls 4.1 库构建（pin 经
# tools/check_dependencies.py 校验——dependencies.json 同源）+
# transport/装配链编。运行（真调用）=手工：./build/f2-cloud/cloud
# （打印 USAGE_TOKENS 行——台账记账用）。
set -e
cd "$(dirname "$0")/.."
OUT=${ENTELEA_CLOUD_OUT:-build/f2-cloud}
MB=${ENTELEA_MBEDTLS_BUILD:-build/.toolobj/mbedtls-host}
M=${ENTELEA_MBEDTLS_SOURCE:-../entelea-legacy/.deps/mbedtls}
P=${ENTELEA_PSA_SOURCE:-../entelea-legacy/.deps/tf-psa-crypto}
mkdir -p "$OUT"

# pin 校验（review P2-4：源树换版必须显式失败，不静默换 TLS 库）。
python3 tools/check_dependencies.py --dependency mbedtls --checkout "$M"
python3 tools/check_dependencies.py --dependency tf_psa_crypto --checkout "$P"
MB_PIN=$(git -C "$M" rev-parse HEAD)
PSA_PIN=$(git -C "$P" rev-parse HEAD)
STAMP="$MB/build-stamp"
if [ ! -f "$MB/library/libmbedtls.a" ] || [ ! -f "$MB/library/libmbedx509.a" ] ||
   [ ! -f "$MB/library/libmbedcrypto.a" ] ||
   [ "$(cat "$STAMP" 2>/dev/null)" != "$MB_PIN/$PSA_PIN" ]; then
  if [ -d "$MB" ] && [ -n "$(find "$MB" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    echo "FAIL: incomplete or mismatched TLS build at $MB; select a new empty ENTELEA_MBEDTLS_BUILD" >&2
    exit 1
  fi
  mkdir -p "$MB"
  cmake -S "$M" -B "$MB" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
    -DGEN_FILES=OFF -DCMAKE_BUILD_TYPE=Release -DMBEDTLS_AS_SUBPROJECT=ON \
    -DTF_PSA_CRYPTO_DIR="$(readlink -f "$P")" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POLICY_DEFAULT_CMP0077=NEW > /dev/null
  cmake --build "$MB" --parallel "$(nproc)" > /dev/null
  echo "$MB_PIN/$PSA_PIN" > "$STAMP"
fi

CC=${CC:-cc}
$CC -std=c11 -Wall -Wextra -Werror \
  -Iframework/include -Iplatform/linux -Iplatform/linux/zephyr_inc \
  -I"$M/include" -I"$P/include" \
  -I"$MB/include" -I"$MB/tf-psa-crypto/include" \
  -I"$P/drivers/builtin/include" -I"$P/core/include" -I"$P/dispatch/include" \
  -DENTELEA_TEST=1 \
  platform/linux/https_chat.c platform/linux/cloud_main.c \
  platform/linux/zephyr_shim.c \
  framework/services/assistant/model.c \
  framework/services/config/d12.c \
  platform/linux/d12_ports.c \
  framework/services/net/netmon.c \
  framework/services/config/prov.c \
  framework/services/storage/part.c \
  framework/services/storage/audit.c \
  framework/execution/safety/channel.c \
  framework/execution/safety/commit.c \
  framework/execution/safety/force.c \
  framework/execution/safety/drivers.c \
  framework/runtime/supervision/events.c \
  framework/runtime/supervision/wdt.c \
  platform/linux/clock.c \
  framework/common/time.c \
  -include platform/linux/zephyr_shim.h \
  -o "$OUT/cloud" "$MB/library/libmbedtls.a" "$MB/library/libmbedx509.a" \
  "$MB/library/libmbedcrypto.a" -lpthread
echo "[f2-cloud] built $OUT/cloud"
