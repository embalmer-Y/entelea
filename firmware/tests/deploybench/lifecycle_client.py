#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""deploybench D8 客户端（单元 I / MD2）：全生命周期混合 demo 网络驱动。

对真板（xiao_esp32s3，WiFi→zenohd@9955）执行三段部署链：
  v1 初装（demo.d8.lifecycle 1.0.0 → slot1）→
  v2 升级（2.0.0 → slot0；**激活即停 v1——G4 语义真机首证**）→
  v2bad 坏版本（2.1.0 → slot1，健康恒病）→ 板自动健康回滚 → v2 复活（slot0，
  rollback_count=1）。

版本区分经 active_slot（get-app 无 app_ver 字段——观测面小缺口登记）；
APP 侧 console 行（d8 v1/v2/bad init）为辅助证据（console 抓取另跑）。
预置：deploybench 已烧录（db_build.sh + db_e2e.sh 同型）；zenohd 由本脚本拉起。

用法（agent-venv，WSL 内）：
  ~/project/agent-venv/bin/python lifecycle_client.py [--router tcp/127.0.0.1:9955]
判定：末行 D8 PASS；任一环节失败非零退出（军规 7）。
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_net import deploy, keys
from tessera_agent.tools_net.zenoh_service import ZenohService

ZENOHD = Path("~/project/tools/zenohd").expanduser()
NODE, CUBE = "dbn", "dbc"
DEMO = REPO / "agent" / "demos" / "demo-lifecycle"
TS_APP_ACTIVE = 3  # ts_app_state_t ACTIVE
HB_KEY = f"tessera/{NODE}/{CUBE}/sys/hb-host"  # linkmon 判活源（DR-12）


def heartbeat(svc: ZenohService, stop: threading.Event) -> None:
    """host 心跳 1s 周期（MD1 教训：不发心跳 = linkmon 永不判活，写全 -4）。"""
    n = 0
    while not stop.is_set():
        n += 1
        try:
            svc.put(HB_KEY, struct.pack(">I", n))
        except Exception:  # noqa: BLE001, S110 —— 会话抖动，下拍再试
            pass
        stop.wait(1.0)

# 部署计划：name → 包目录（期望 slot 动态链式计算：v1 = 起始 active^1，
# 后续 = 前者^1——对任意起始 meta 态幂等可重跑）
PLAN = [
    ("v1", DEMO / "v1"),      # 初装
    ("v2", DEMO / "v2"),      # 升级（**激活即停 v1——G4 语义真机首证**）
    ("v2bad", DEMO / "v2bad"),  # 坏版本（健康恒病 → 自动回滚）
]


def log(msg: str) -> None:
    print(f"D8 {msg}", flush=True)


def ensure_zenohd(port: int) -> subprocess.Popen | None:
    with socket.socket() as s:
        s.settimeout(1)
        if s.connect_ex(("127.0.0.1", port)) == 0:
            return None
    p = subprocess.Popen([str(ZENOHD), "--listen", f"tcp/0.0.0.0:{port}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    for _ in range(50):
        with socket.socket() as s:
            s.settimeout(1)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return p
        time.sleep(0.2)
    p.terminate()
    raise SystemExit("D8 FAIL zenohd 未就绪")


def find_cube(svc: ZenohService, timeout_s: float) -> dict:
    """发现 dbn/dbc——通配发现失败时兜底点对点直查（B2 实测：暖复位后
    路由器残留陈旧 queryable 声明〔死 peer 各拖 3s 超时〕，通配合并可
    超时而直查单路可达；D8 的 node/cube 已知，无需通配）。"""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            cubes = deploy.discover(svc, timeout_s=3.0)
            for c in cubes:
                if c["node_id"] == NODE and c["cube_id"] == CUBE:
                    return c
        except Exception:  # noqa: BLE001, S110 —— 重启窗口 zenoh抖动兜底
            pass
        try:
            _, pl = svc.query_one(f"tessera/{NODE}/{CUBE}/sys/get-info",
                                  keys.envelope("get-info", "fc"),
                                  timeout_s=4.0)
            r = keys.parse_reply(pl)
            if r.data.get("node") == NODE and r.data.get("cube") == CUBE:
                return {"node_id": NODE, "cube_id": CUBE,
                        "fw": r.data.get("fw"), "board": r.data.get("board"),
                        "status": r.status}
        except Exception:  # noqa: BLE001, S110 —— 直查兜底亦失败，下轮再试
            pass
        time.sleep(1.0)
    raise SystemExit(f"D8 FAIL 未发现 {NODE}/{CUBE}（{timeout_s}s）")


def get_app(svc: ZenohService) -> dict:
    return deploy.status(svc, NODE, CUBE)["get-app"]


def wait_for(svc: ZenohService, want_slot: int, want_state: int,
             min_rollback: int, timeout_s: float, tag: str) -> dict:
    """轮询 get-app 至期望态（容忍重启窗口的发现抖动与中间态）。"""
    deadline = time.monotonic() + timeout_s
    ga: dict = {}
    while time.monotonic() < deadline:
        try:
            ga = get_app(svc)
            if (int(ga.get("active_slot", -1)) == want_slot and
                    int(ga.get("state", -1)) == want_state and
                    int(ga.get("rollback_count", 0)) >= min_rollback):
                return ga
        except Exception:  # noqa: BLE001, S110 —— 板暖复位中
            pass
        time.sleep(1.5)
    raise SystemExit(f"D8 FAIL {tag}: {timeout_s}s 内未达 slot={want_slot} "
                     f"state={want_state} rc>={min_rollback}（last={ga}）")


def deploy_one(svc: ZenohService, name: str, pkg_dir: Path,
               want_slot: int, pub: Path, wait_active: bool = True) -> None:
    pkg = next(pkg_dir.glob("*.tsap"))
    out = deploy.push_app(svc, NODE, CUBE, str(pkg), str(pub),
                          chunk_size=256, log_fn=log)
    assert out["confirmed"] is True and out["released"] is True, json.dumps(out)
    assert int(out["active_slot"]) == want_slot, \
        f"{name}: 目标槽 {out['active_slot']} != 预期 {want_slot}"
    log(f"{name} 推送完成 slot={out['active_slot']}（激活即停旧版——G4）")
    if not wait_active:
        return  # v2bad：瞬态 ACTIVE ~3s 即被健康回滚翻转——直接等终态
    time.sleep(8.0)  # DB4/DB5 板侧 2s 观测 + 暖复位 + 装载
    find_cube(svc, 60.0)
    ga = wait_for(svc, want_slot, TS_APP_ACTIVE, 0, 45.0, f"{name} 运行")
    log(f"{name} ACTIVE slot={ga['active_slot']} rc={ga.get('rollback_count')}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--router", default="tcp/127.0.0.1:9955")
    args = ap.parse_args()

    pub = DEMO / "v1" / "test-root.pub"  # 三包同根（ts-test-root）
    for _n, d in PLAN:
        assert next(d.glob("*.tsap")).is_file(), f"包缺失：{d}"

    zenohd = ensure_zenohd(int(args.router.rsplit(":", 1)[1]))
    hb_stop = threading.Event()
    try:
        with (tempfile.TemporaryDirectory(prefix="d8-") as _td,
              ZenohService(args.router, query_timeout_s=5.0) as svc):
            hb = threading.Thread(target=heartbeat, args=(svc, hb_stop),
                                  daemon=True)
            hb.start()
            find_cube(svc, 90.0)
            before = get_app(svc)
            slot = int(before.get("active_slot", 0))
            log(f"发现 {NODE}/{CUBE}；起始 active_slot={slot}（后续链式取反）")

            for name, pkg_dir in PLAN:
                slot ^= 1  # 目标槽 = 当前 active ^ 1
                deploy_one(svc, name, pkg_dir, slot, pub,
                           wait_active=(name != "v2bad"))
                if name == "v1":
                    log("D8a v1 初装运行（console: d8 v1 init / D8-DONE）")
                elif name == "v2":
                    log("D8b v2 升级运行（console: d8 v2 init / D8-DONE）")
                else:
                    # 坏版本：装载 → 健康探针 3 败 → 自动回滚（目标槽 v2 验签
                    # 通过）→ 暖复位 → v2 复活。瞬态 ACTIVE ~3s 不作硬判据
                    # （B2 实测：轮询窗错过瞬态 ≠ 失败——终态才是判据）。
                    log("D8c-1 v2bad 已激活（console: d8bad init；~4s 健康回滚）")
                    time.sleep(15.0)  # 探针 3×1000ms + rollback 验签 + 暖复位
                    find_cube(svc, 90.0)
                    ga = wait_for(svc, slot ^ 1, TS_APP_ACTIVE, 1, 60.0,
                                  "回滚复活 v2")
                    log(f"D8c-2 v2 复活 slot={ga['active_slot']} "
                        f"rollback_count={ga.get('rollback_count')}")
    finally:
        hb_stop.set()
        if zenohd is not None:
            zenohd.terminate()
    print("D8 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
