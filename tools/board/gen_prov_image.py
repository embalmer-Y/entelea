#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""gen_prov_image.py — atkbench prov 数组生成（军规 27 机械生成纪律）。
v2 12 对形态（F3 · DEC-62①）：v1 前缀 + deploy_mode/estop_remote_clear + lconf。
estop_remote_clear=0（本地清除板）+ lconf=1（安装闸开——F3 真机判据载体）。"""
import cbor2

prov = {
    "v": 2,
    "node_id": "akn",
    "cube_id": "akc",
    "routers": ["tcp/192.168.2.91:9955"],
    "pk0": b"\x00" * 32,
    "pk1": b"\x00" * 32,
    "cred": "",
    "pwr_ma": 500,
    "estop": 0,
    "deploy_mode": 0,
    "estop_remote_clear": 0,
    "lconf": 1,
}
blob = cbor2.dumps(prov)
# 固件 prov_walk 定序校验（12 键序 = v1 前缀 + deploy_mode/estop_remote_clear/lconf）
print("/* 机械生成（gen_prov_image.py——重跑复现；禁手抄，教训 27） */")
print("static const uint8_t prov_blob[] = {")
for i in range(0, len(blob), 12):
    print("\t" + ", ".join(f"0x{b:02x}" for b in blob[i:i+12]) + ",")
print("};")
print(f"/* {len(blob)}B（走查序由 ts_store_prov_validate 校验） */")
