# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""dav_recv（V0.4.0 M11 重建——原 tools/board 版丢失）：demo-audio-video（原 D-AV）帧重组消费端。

订阅 `tessera/*/*/av/*/frame` 分片（CBOR 信封 {fid,cid,n,crc,d}——avq.c 5 对
canonical map），按 fid 收集全部分片 → IEEE crc32 校验 → 重组落盘。
QoS = BESTEFFORT（V2-1 定版）——分片丢失容忍（超时未齐即弃帧）。

用法：agent-venv python dav_recv.py [--router tcp/127.0.0.1:9955]
     [--out DIR] [--count N] [--timeout S]
判据（DA PASS 形态）：≥N 帧完整重组落盘（crc 全过）。
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
import zlib
from pathlib import Path

import cbor2
import zenoh


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--router", default="tcp/127.0.0.1:9955")
    ap.add_argument("--out", default=Path("~/project/logs/dav_frames").expanduser())
    ap.add_argument("--count", type=int, default=2, help="目标完整帧数")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    frames: dict[int, dict[int, bytes]] = {}
    meta: dict[int, int] = {}  # fid -> n
    done: list[tuple[int, int]] = []  # (fid, size)
    corrupt = 0
    deadline = time.monotonic() + args.timeout

    conf = zenoh.Config()
    locator = args.router.replace("://", "/")  # B2 批修正：1.10.1 正式语法
    conf.insert_json5("connect", json.dumps({"endpoints": [locator]}))
    session = zenoh.open(conf)
    try:
        def on_sample(sample):
            nonlocal corrupt
            try:
                env = cbor2.loads(bytes(sample.payload))
                fid = int(env["fid"])
                cid = int(env["cid"])
                n = int(env["n"])
                crc = int(env["crc"])
                d = bytes(env["d"])
            except Exception:
                corrupt += 1
                return
            # 合同（avq.c）：crc = crc32(单分片)——逐分片校验（整帧完整性 =
            # n 片齐且各片 crc 过；M18 真机实证修正重建件的整帧误校验）
            if crc32_ieee(d) != crc:
                corrupt += 1
                print(f"frame {fid} chunk {cid}: CRC 不符（弃片）", flush=True)
                return
            fr = frames.setdefault(fid, {})
            if meta.setdefault(fid, n) != n:
                corrupt += 1  # 同 fid 分片数不一致（发送端异常形态）
                return
            fr[cid] = d
            if len(fr) == n:
                blob = b"".join(fr[c] for c in range(n))
                out = args.out / f"frame_{fid}_{len(blob)}B.rgb565"
                out.write_bytes(blob)
                done.append((fid, len(blob)))
                print(f"frame {fid}: 完整 {len(blob)}B → {out}", flush=True)
                frames.pop(fid, None)
                meta.pop(fid, None)

        session.declare_subscriber("tessera/*/*/av/*/frame", on_sample)
        print(f"=== dav_recv 订阅中（router={args.router}，目标 {args.count} 帧，"
              f"超时 {args.timeout}s） ===", flush=True)
        while len(done) < args.count and time.monotonic() < deadline:
            time.sleep(0.2)

    finally:
        session.close()
    print(f"=== 完成：{len(done)} 帧完整（{done}）；损坏/异常 {corrupt} ===",
          flush=True)
    return 0 if len(done) >= args.count else 1


sys.exit(main())
