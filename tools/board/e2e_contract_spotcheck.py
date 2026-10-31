#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""e2e_contract_spotcheck.py — V0.7.0 真机 spot check 驱动（仓外惯例）。

与 client.py 同型，但签名钥 = 固定测试根（agent/tests/fixtures/ts-test-root）
——DEC-49 后 TEST 语义板（deploybench）验签对象 = 固定测试根，临时钥对
必拒。判定输出末行 CG7 DEPLOY PASS。
"""
from __future__ import annotations

import json
import sys
import tempfile
import time
from pathlib import Path

REPO = Path("/home/emb/project/Tessera")
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_net import deploy  # noqa: E402
from tessera_agent.tools_net.zenoh_service import ZenohService  # noqa: E402
from tessera_agent.tools_tsap import tools as tsap_tools  # noqa: E402

NODE, CUBE = "dbn", "dbc"
WASM = REPO / "firmware" / "tests" / "app" / "appw" / "native_app.wasm"
TEST_ROOT_KEY = AGENT / "tests" / "fixtures" / "ts-test-root.key"
TEST_ROOT_PUB = AGENT / "tests" / "fixtures" / "ts-test-root.pub"
MANIFEST = {
    "app_id": "com.tessera.cg7", "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0", "caps": ["gpio:write:0-3"],
    "stack_kb": 4, "heap_kb": 16,
    "exports": ["health_ping", "app_init", "app_evt"],
}
TS_APP_ACTIVE = 3


def log(msg: str) -> None:
    print(f"CG7 {msg}", flush=True)


def find_cube(svc: ZenohService, timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for c in deploy.discover(svc, timeout_s=3.0):
            if c.get("node_id") == NODE and c.get("cube_id") == CUBE:
                return c
        time.sleep(1.0)
    raise SystemExit(f"CG7 FAIL 未发现 {NODE}/{CUBE}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cg7-") as td:
        tmp = Path(td)
        with ZenohService("tcp/127.0.0.1:9955", query_timeout_s=5.0) as svc:
            cube = find_cube(svc, 60.0)
            log(f"发现 fw={cube.get('fw')} board={cube.get('board')}")
            caps = cube.get("caps")
            log(f"caps 块（CG5-A 实机自报）: {json.dumps(caps, ensure_ascii=False)}")

            before = deploy.status(svc, NODE, CUBE)
            log(f"部署前 get-app: {json.dumps(before['get-app'], ensure_ascii=False)}")

            pkg = tsap_tools.tsap_package(
                str(WASM), MANIFEST, str(TEST_ROOT_KEY), str(tmp),
                [str(tmp), str(WASM.parent)])
            log(f"TSAP 打包（测试根签名）: {pkg['package_path']} {pkg['size']}B")

            out = deploy.push_app(svc, NODE, CUBE, pkg["package_path"],
                                  str(TEST_ROOT_PUB), chunk_size=256,
                                  log_fn=log)
            assert out["confirmed"] is True, json.dumps(out, default=str)
            assert out["released"] is True
            log(f"推送完成: chunks={out['chunks']} slot={out['active_slot']} "
                f"device_caps={out.get('device_caps')}")

            log("等待板暖复位 + APP 装载…")
            time.sleep(12.0)
            find_cube(svc, 90.0)
            after = deploy.status(svc, NODE, CUBE)
            ga = after["get-app"]
            log(f"部署后 get-app: {json.dumps(ga, ensure_ascii=False)}")
            assert int(ga["active_slot"]) == int(out["active_slot"]), ga
            assert int(ga["state"]) == TS_APP_ACTIVE, ga
            assert ga["app_id"] == MANIFEST["app_id"], ga
    print("CG7 DEPLOY PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
