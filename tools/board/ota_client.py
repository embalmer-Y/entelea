# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""ota_client（V0.4.0 M11 重建——原 tools/board 版丢失）：MCUmgr SMP UDP 客户端。

Zephyr smp_udp（otabench :1337）图像上传链：分块 upload → test 标记 →
（复位 swap 验证后）confirm 确认。协议 = MCUmgr SMP（8B 头 op|flags|len|seq|
group|id + CBOR；image group=1：upload id=1 / state id=0）。

用法：agent-venv python ota_client.py upload <板IP:1337> <image.bin>
                    test <板IP:1337> --hash <hex>
                    confirm <板IP:1337> --hash <hex>
判据（G2 形态）：upload 完成回 off==total；test/confirm 回 rc=0。
注：协议为 mcumgr 规范面的最小实现（重建件）——真机链路验证随 otabench
板级复跑批（V0.4.0 收口前）。
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

import cbor2

OP_WRITE = 2
OP_WRITE_RSP = 3
GROUP_IMAGE = 1
IMG_UPLOAD = 1
IMG_STATE = 0
CHUNK = 384  # 原 client 实证档（646676B / ~1685 包）
RETRY_MAX = 5
RETRY_WAIT_S = 0.3
MGMT_ERR_EOK = 0


class SmpClient:
    def __init__(self, addr: str, timeout_s: float = 5.0):
        host, port = addr.rsplit(":", 1)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout_s)
        self.peer = (host, int(port))
        self.seq = 0

    def _send_recv(self, payload: dict) -> dict:
        body = cbor2.dumps(payload)
        self.seq = (self.seq + 1) & 0xFF
        hdr = struct.pack(">BBHBHB", OP_WRITE, 0, len(body), self.seq,
                          payload["_group"], payload["_id"])
        frame = hdr + body
        for attempt in range(RETRY_MAX):
            self.sock.sendto(frame, self.peer)
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            op, _flags, plen, seq, _group, _id = struct.unpack(">BBHBHB", data[:8])
            if op != OP_WRITE_RSP or seq != self.seq or plen != len(data) - 8:
                continue
            return cbor2.loads(data[8:])
        msg = f"SMP 无有效回执（seq={self.seq} 重试 {RETRY_MAX} 次）"
        raise RuntimeError(msg)

    def upload(self, image: bytes) -> dict:
        total = len(image)
        sent = 0
        first = True
        while sent < total:
            data = image[sent:sent + CHUNK]
            req: dict = {"_group": GROUP_IMAGE, "_id": IMG_UPLOAD,
                         "data": data, "len": total}
            if not first:
                req["off"] = sent
            rep = self._send_recv(req)
            rc = rep.get("rc", MGMT_ERR_EOK)
            if rc != MGMT_ERR_EOK:
                msg = f"upload 包 rc={rc}"
                raise RuntimeError(msg)
            nxt = int(rep.get("off", len(data) if first else sent + len(data)))
            if nxt <= sent:
                msg = f"offset 无进展（{sent} → {nxt}）"
                raise RuntimeError(msg)
            sent = nxt
            first = False
            print(f"  upload off={sent}/{total}", flush=True)
        return {"off": sent, "len": total}

    def state(self, image_sha: bytes, *, confirm: bool) -> dict:
        req: dict = {"_group": GROUP_IMAGE, "_id": IMG_STATE, "hash": image_sha}
        if confirm:
            req["confirm"] = True
        else:
            req["test"] = True
        rep = self._send_recv(req)
        rc = rep.get("rc", MGMT_ERR_EOK)
        if rc != MGMT_ERR_EOK:
            msg = f"state rc={rc}"
            raise RuntimeError(msg)
        return rep


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["upload", "test", "confirm"])
    ap.add_argument("addr", help="板 IP:端口（smp_udp，otabench 默认 :1337）")
    ap.add_argument("image", nargs="?", help="upload：镜像 .bin 路径")
    ap.add_argument("--hash", help="test/confirm：镜像 sha256（hex）")
    args = ap.parse_args()

    cli = SmpClient(args.addr)
    t0 = time.monotonic()
    if args.cmd == "upload":
        blob = open(args.image, "rb").read()
        r = cli.upload(blob)
        print(f"upload PASS {r['off']}/{r['len']}B "
              f"({time.monotonic() - t0:.1f}s)")
        return 0
    if not args.hash:
        print("test/confirm 需要 --hash")
        return 2
    sha = bytes.fromhex(args.hash)
    r = cli.state(sha, confirm=(args.cmd == "confirm"))
    print(f"{args.cmd} PASS {r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
