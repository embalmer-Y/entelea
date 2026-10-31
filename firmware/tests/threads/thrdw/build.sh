#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# framework.threads 夹具构建：WAMR spawn-thread 样例同款线程档旗标
# （samples/spawn-thread/wasm-apps/CMakeLists.txt 配方）——共享内存 +
# 原子（-pthread）+ aux stack（-z stack-size）+ heap/data 导出（WAMR
# aux stack/堆探测约定）。thread_app.c 变更后重跑并提交产物。
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -pthread \
      -z stack-size=32768 \
      -Wl,--no-entry -Wl,--allow-undefined -Wl,--strip-all \
      -Wl,--shared-memory -Wl,--max-memory=131072 \
      -Wl,--export=health_ping -Wl,--export=app_init -Wl,--export=app_tick \
      -Wl,--export=worker \
      -Wl,--export=__heap_base -Wl,--export=__data_end \
      -o thread_app.wasm thread_app.c
echo "thread_app.wasm: $(wc -c < thread_app.wasm) bytes"
