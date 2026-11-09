#!/bin/sh
# F-2 真云闸（Linux host）：mbedtls 4.1 库构建（pin 经
# tools/check_dependencies.py 校验——dependencies.json 同源）+
# transport/装配链编。运行（真调用）=手工：./build/f2-cloud/cloud
# （打印 USAGE_TOKENS 行——台账记账用）。
set -e
cd "$(dirname "$0")/.."
OUT=build/f2-cloud
MB=build/.toolobj/mbedtls-host
M=../entelea-legacy/.deps/mbedtls
P=../entelea-legacy/.deps/tf-psa-crypto
mkdir -p "$OUT" "$MB"

# pin 校验（review P2-4：源树换版必须显式失败，不静默换 TLS 库）。
python3 tools/check_dependencies.py > /dev/null
MB_PIN=$(git -C "$M" rev-parse HEAD)
PSA_PIN=$(git -C "$P" rev-parse HEAD)
check_pin() { # id actual
  want=$(python3 -c "import json;print([d['commit'] for d in json.load(open('build-support/dependencies.json'))['dependencies'] if d['id']=='$1'][0])")
  [ "$2" = "$want" ] || { echo "FAIL: $1 pin mismatch ($2 != $want)"; exit 1; }
}
check_pin mbedtls "$MB_PIN"
check_pin tf_psa_crypto "$PSA_PIN"
STAMP="$MB/build-stamp"
if [ ! -f "$MB/library/libmbedtls.a" ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$MB_PIN/$PSA_PIN" ]; then
  rm -rf "$MB"
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
