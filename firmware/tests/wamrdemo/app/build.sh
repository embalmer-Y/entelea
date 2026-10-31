#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Q-23 实验批负载构建：clang wasm32 自由固件（无 WASI/无 libc，DEC-25）。
# 产物 busy.wasm = framework.wamrdemo 夹具；busy.c 变更后重跑并提交。
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin \
      -Wl,--no-entry -Wl,--strip-all \
      -o busy.wasm busy.c
echo "busy.wasm: $(wc -c < busy.wasm) bytes"
