#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""deploybench 客户端（板级十）：Agent→真机完整部署 E2E 驱动。

复用 Agent 侧 deploy 链（MA3.1 与 sim E2E 同一实现——tools_net.deploy +
ZenohService + tools_tsap），对真板（xiao_esp32s3，WiFi→zenohd@9955）执行：
发现 → 状态核验 → keygen/打包 → 分块推送（租约闭环 + verify + activate +
get-app 确认）→ 等板自动暖复位 → 复位后 get-app 对拍（state=ACTIVE +
app_id）。

用法（agent-venv，WSL 内）：
  ~/project/agent-venv/bin/python client.py [--router tcp/127.0.0.1:9955]

判定：输出末行 DEPLOY PASS；任一环节失败非零退出（军规 7 如实报告）。
"""
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_net import deploy  # noqa: E402
from tessera_agent.tools_net.zenoh_service import ZenohService  # noqa: E402
from tessera_agent.tools_tsap import tools as tsap_tools  # noqa: E402

ZENOHD = Path("~/project/tools/zenohd").expanduser()
NODE, CUBE = "dbn", "dbc"
WASM = REPO / "firmware" / "tests" / "app" / "appw" / "native_app.wasm"
MANIFEST = {
    "app_id": "com.tessera.e2e", "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0", "caps": ["gpio:write:0-3"],
    "stack_kb": 4, "heap_kb": 16,
    "exports": ["health_ping", "app_init", "app_evt"],
}
TS_APP_ACTIVE = 3  # ts_app_state_t（appmgr.h 六态：RECEIVED=0/VERIFIED=1/
                   # STAGED=2/ACTIVE=3/ROLLBACK=4/QUARANTINED=5）
CHUNK = 256  # 多分块真机证据（~1KB 包 → 4 块；默认 2048 会单块直过）


def log(msg: str) -> None:
    print(f"DEPLOY {msg}", flush=True)


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
    raise SystemExit("DEPLOY FAIL zenohd 未就绪")


def find_cube(svc: ZenohService, timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        cubes = deploy.discover(svc, timeout_s=3.0)
        for c in cubes:
            if c["node_id"] == NODE and c["cube_id"] == CUBE:
                return c
        time.sleep(1.0)
    raise SystemExit(f"DEPLOY FAIL 未发现 {NODE}/{CUBE}（{timeout_s}s）")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--router", default="tcp/127.0.0.1:9955")
    ap.add_argument("--boot-wait-s", type=float, default=90.0)
    args = ap.parse_args()

    port = int(args.router.rsplit(":", 1)[1])
    zenohd = ensure_zenohd(port)
    assert WASM.is_file(), f"夹具 wasm 缺失：{WASM}"

    with tempfile.TemporaryDirectory(prefix="ts-deploy-") as td:
        tmp = Path(td)
        with ZenohService(args.router, query_timeout_s=5.0) as svc:
            cube = find_cube(svc, args.boot_wait_s)
            log(f"发现 {cube['node_id']}/{cube['cube_id']} fw={cube.get('fw')}")
            before = deploy.status(svc, NODE, CUBE)
            log(f"部署前 get-app: {json.dumps(before['get-app'], ensure_ascii=False)}")

            tsap_tools.tsap_keygen("e2e", str(tmp), [str(tmp)])
            pkg = tsap_tools.tsap_package(
                str(WASM), MANIFEST, str(tmp / "e2e.key"), str(tmp), [str(tmp)])
            log(f"TSAP 打包: {pkg['package_path']}")

            out = deploy.push_app(svc, NODE, CUBE, pkg["package_path"],
                                  str(tmp / "e2e.pub"), chunk_size=CHUNK,
                                  log_fn=log)
            assert out["confirmed"] is True, json.dumps(out, default=str)
            assert out["released"] is True
            log(f"推送完成: chunks={out['chunks']} size={out['size']} "
                f"slot={out['active_slot']}")

            # 板侧激活后自动暖复位（DB4/DB5）→ 等 rediscover + APP 运行
            log("等待板暖复位 + APP 装载（DB6）…")
            time.sleep(10.0)
            find_cube(svc, args.boot_wait_s)
            after = deploy.status(svc, NODE, CUBE)
            ga = after["get-app"]
            log(f"部署后 get-app: {json.dumps(ga, ensure_ascii=False)}")
            assert int(ga["active_slot"]) == int(out["active_slot"]), ga
            assert int(ga["state"]) == TS_APP_ACTIVE, ga
            assert ga["app_id"] == MANIFEST["app_id"], ga
    if zenohd is not None:
        zenohd.terminate()
    print("DEPLOY PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
