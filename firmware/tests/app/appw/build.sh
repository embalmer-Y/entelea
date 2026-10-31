#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# framework.app 夹具构建：clang wasm32 自由固件（导入 ts_api_v1 natives；
# 未授权符号在本夹具全量声明——权限由调用期裁决拦截，DEC-25 边界不依赖
# 链接期装配）。native_app.c 变更后重跑并提交产物。
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin \
      -Wl,--no-entry -Wl,--allow-undefined -Wl,--strip-all \
      -o native_app.wasm native_app.c
echo "native_app.wasm: $(wc -c < native_app.wasm) bytes"
