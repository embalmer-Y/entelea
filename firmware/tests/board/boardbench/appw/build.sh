#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# boardbench 夹具构建：clang wasm32 自由固件（同 tests/app/appw 模式；
# 未授权符号全量声明，权限由调用期裁决拦截）。bench.c 变更后重跑并提交产物。
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin \
      -Wl,--no-entry -Wl,--allow-undefined -Wl,--strip-all \
      -o bench.wasm bench.c
echo "bench.wasm: $(wc -c < bench.wasm) bytes"
