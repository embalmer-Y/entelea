#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P4 联网批 E1/E2 数据面对拍器：UDP 回声器（board 侧 p4wifibench 对拍）。

用法：python3 tools/board/p4_udp_echo.py [--port 9999] [--bind 0.0.0.0]
每收一包：打时间戳 + 来源 + 内容 → 原样回发（板侧判据 P4W1 UDP RTT OK）。
证据纪律：内容含板侧 seq，无凭据面——可全文入 evidence。
"""
import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9999)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.bind, args.port))
    print(f"udp echo listening on {args.bind}:{args.port}", flush=True)
    while True:
        data, addr = s.recvfrom(2048)
        print(f"{time.strftime('%H:%M:%S')} <- {addr[0]}:{addr[1]} {data!r}",
              flush=True)
        s.sendto(data, addr)


if __name__ == "__main__":
    sys.exit(main())
