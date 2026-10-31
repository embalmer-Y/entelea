#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# 样例 APP 构建（M2b.2a）：clang wasm32 自由固件——无 WASI/无 libc（DEC-25）。
# 产物 sample.wasm = framework.wamr 测试夹具；sample.c 变更后重跑本脚本并提交产物。
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin \
      -Wl,--no-entry -Wl,--strip-all \
      -o sample.wasm sample.c
echo "sample.wasm: $(wc -c < sample.wasm) bytes"
