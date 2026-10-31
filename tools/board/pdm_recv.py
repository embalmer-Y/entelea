# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""pdm_recv（V2 纵深 M18）：demo-pdm-audio（原 D-PDM）帧重组消费端——pcm16 音频落盘。

订阅 `tessera/*/*/av/*/frame` 分片（CBOR 信封 {fid,cid,n,crc,d}——avq.c 5 对
canonical map），按 fid 收集全部分片 → IEEE crc32 校验 → 重组 → 依 fid 序
落盘 .pcm + .wav（16kHz mono s16le）+ 非平坦内容判定。
QoS = BESTEFFORT（V2-1 定版）——分片丢失容忍（超时未齐即弃帧）。

用法：agent-venv python pdm_recv.py [--router tcp/127.0.0.1:9955]
     [--out DIR] [--count N] [--timeout S]
判据（DP PASS 形态，与 console 判据独立双证）：≥N 帧完整重组（crc 全过）
且样本内容非平坦（span > 阈值——静音板/桩数据鉴别）。
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
import wave
import zlib
from pathlib import Path

import cbor2
import zenoh

FS_HZ = 16000
FRAME_BYTES = 640
SPAN_MIN = 100  # 非平坦判定：s16 样本 max-min 至少 100（PDMB 实测 ~5000）


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--router", default="tcp/127.0.0.1:9955")
    ap.add_argument("--out", default=Path("~/project/logs/dpm_frames").expanduser())
    ap.add_argument("--count", type=int, default=8, help="目标完整帧数")
    ap.add_argument("--timeout", type=float, default=90.0)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    frames: dict[int, dict[int, bytes]] = {}
    meta: dict[int, int] = {}  # fid -> n
    done: dict[int, bytes] = {}  # fid -> blob
    corrupt = 0
    deadline = time.monotonic() + args.timeout

    conf = zenoh.Config()
    locator = args.router.replace("://", "/")  # 1.10.1 正式语法（B2 修正）
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
                done[fid] = blob
                print(f"frame {fid}: 完整 {len(blob)}B", flush=True)
                frames.pop(fid, None)
                meta.pop(fid, None)

        session.declare_subscriber("tessera/*/*/av/*/frame", on_sample)
        print(f"=== pdm_recv 订阅中（router={args.router}，目标 {args.count} 帧，"
              f"超时 {args.timeout}s） ===", flush=True)
        while len(done) < args.count and time.monotonic() < deadline:
            time.sleep(0.2)

    finally:
        session.close()

    fids = sorted(done)
    pcm = b"".join(done[f] for f in fids)
    pcm_path = args.out / f"demo_dpm_{len(fids)}f_{len(pcm)}B.pcm"
    wav_path = args.out / "demo_dpm.wav"
    pcm_path.write_bytes(pcm)
    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS_HZ)
        w.writeframes(pcm)

    span = 0
    if pcm:
        samples = struct.unpack(f"<{len(pcm) // 2}h", pcm[: (len(pcm) // 2) * 2])
        span = max(samples) - min(samples)
    ok = len(fids) >= args.count and span >= SPAN_MIN
    print(f"=== 完成：{len(fids)} 帧完整（fid={fids}）；损坏 {corrupt}；"
          f"span={span}（阈值 {SPAN_MIN}） ===", flush=True)
    print(f"=== 落盘：{pcm_path} / {wav_path} ===", flush=True)
    print(f"=== 判据：{'DP PASS（PC 侧）' if ok else 'DP FAIL（PC 侧）'} ===",
          flush=True)
    return 0 if ok else 1


sys.exit(main())
