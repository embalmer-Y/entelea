#!/usr/bin/env python3
"""netbench 客户端（板级四单元）：WiFi + zenoh 命令往返时延测量。

三层探针（agent 侧 eclipse-zenoh，经本机 zenohd router）：
  L0 router-local echo：客户端 put → router → 客户端 sub（纯 PC 侧跳，
     无设备——从设备 RTT 中扣除得"无线+设备"净耗时）；
  L1 sys 查询往返：GET tessera/nbn/cbn/sys/get-info（v2 信封）——
     完整命令面（WiFi→zenoh→CBOR→框架状态→回包）；
  L2 重命令路径：estop-clear 错令牌（安全层交互 + 稳定 PARAM 错回包，
     状态不变可重复）。
各 N 次取 min/p50/p95/max（ms）。用法：
  ~/project/agent-venv/bin/python netbench_client.py [N]
"""
from __future__ import annotations

import json
import statistics
import sys
import threading
import time

import cbor2
import zenoh

Z = "tessera/nbn/cbn"
ENDPOINT = "tcp/127.0.0.1:9955"
DIAG = "--diag" in sys.argv


def tstr(s: str) -> bytes:
    b = s.encode()
    assert len(b) < 24
    return bytes([0x60 | len(b)]) + b


def req2(op: str, rid: str) -> bytes:
    """v2 请求 map{ver,kind,rid,src,op}（DEC-40；同 netlab_client 定序）。"""
    return (bytes([0xA5]) + tstr("ver") + bytes([2]) + tstr("kind")
            + bytes([0x10]) + tstr("rid") + tstr(rid) + tstr("src")
            + tstr("nb-client") + tstr("op") + tstr(op))


def stats(ms: list[float]) -> dict:
    ms2 = sorted(ms)
    return {
        "n": len(ms2),
        "min": round(ms2[0], 3),
        "p50": round(statistics.median(ms2), 3),
        "p95": round(ms2[int(len(ms2) * 95 // 100)], 3),
        "max": round(ms2[-1], 3),
    }


def wait_discovery(seen: dict[str, int], deadline_s: float = 25.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < deadline_s:
        if seen.get(f"{Z}/sys/hb"):
            return True
        time.sleep(0.2)
    return False


def main() -> int:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    conf = zenoh.Config()
    conf.insert_json5("connect", json.dumps({"endpoints": [ENDPOINT]}))
    s = zenoh.open(conf)

    seen: dict[str, int] = {}

    def on_sample(sample) -> None:
        seen[str(sample.key_expr)] = seen.get(str(sample.key_expr), 0) + 1

    s.declare_subscriber("tessera/**", handler=on_sample)

    if not wait_discovery(seen):
        print(json.dumps({"error": "discovery timeout（固件心跳未达——查固件侧 NB* 日志）"}))
        return 1
    print(json.dumps({"discovery_s": 0, "hb": seen.get(f"{Z}/sys/hb", 0),
                      "stage": "discovered"}), flush=True)

    # ---- L0 router-local echo ----
    ev = threading.Event()

    def on_echo(sample) -> None:
        ev.set()

    s.declare_subscriber("nb/base/echo", handler=on_echo)
    l0: list[float] = []
    for i in range(n + 5):
        t0 = time.perf_counter()
        s.put("nb/base/echo", b"ping")
        if not ev.wait(timeout=2.0):
            print(json.dumps({"error": "L0 timeout"}))
            return 1
        if i >= 5:
            l0.append((time.perf_counter() - t0) * 1000)
        ev.clear()

    # ---- L1 sys 查询往返（轻命令）----
    l1: list[float] = []
    l1_fail: list[float] = []
    for i in range(n + 5):
        t0 = time.perf_counter()
        ok = False
        replies = s.get(f"{Z}/sys/get-info", payload=req2("get-info", f"nb-{i}"))
        for r in replies:
            if r.ok is not None:
                ok = True
                break
        dt = (time.perf_counter() - t0) * 1000
        if not ok and not DIAG:
            print(json.dumps({"error": f"L1 no reply @{i}", "dt_ms": round(dt, 3)}))
            return 1
        if ok and i >= 5:
            l1.append(dt)
        elif not ok:
            l1_fail.append(round(dt, 3))

    # ---- L2 重命令路径（estop-clear 错令牌：安全层交互，状态不变）----
    bad = (bytes([0xA6]) + tstr("ver") + bytes([2]) + tstr("kind") + bytes([0x10])
           + tstr("rid") + tstr("nb-bad") + tstr("src") + tstr("nb-client")
           + tstr("op") + tstr("estop-clear") + tstr("args")
           + bytes([0xA1]) + tstr("token") + tstr("wrong-token"))
    l2: list[float] = []
    for i in range(n + 5):
        t0 = time.perf_counter()
        ok = False
        replies = s.get(f"{Z}/sys/estop-clear", payload=bad)
        for r in replies:
            if r.ok is not None:
                ok = True
                break
        dt = (time.perf_counter() - t0) * 1000
        if not ok:
            print(json.dumps({"error": f"L2 no reply @{i}", "dt_ms": round(dt, 3)}))
            return 1
        if i >= 5:
            l2.append(dt)

    out = {"l0_router_local": stats(l0), "l1_sys_query": stats(l1),
           "l1_fail_n": len(l1_fail), "l1_fail_dts": l1_fail[:8],
           "l2_estop_clear": stats(l2)}
    net = {"l1_minus_l0_p50": round(out["l1_sys_query"]["p50"] - out["l0_router_local"]["p50"], 3)}
    print(json.dumps({"result": "PASS", **out, "net": net}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
