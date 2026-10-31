#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""e2e_ln_fanout.py — K6 多真机 LN E2E 驱动（仓外惯例）。

双 cube（dbn/dbc @ xiao + akn/akc @ atk）LN 组：ln_create → ln_deploy 扇出
（同包双板）→ ln_status 聚合（双 ACTIVE）→ 双板 get-app 断言。
末行 AK6 LN PASS 判定。
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

REPO = Path("/home/emb/project/Tessera")
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_net import deploy  # noqa: E402
from tessera_agent.tools_net import keys as K  # noqa: E402
from tessera_agent.tools_net.lno import LnRegistry, ln_deploy, ln_status  # noqa: E402
from tessera_agent.tools_net.zenoh_service import ZenohService  # noqa: E402
from tessera_agent.tools_tsap import tools as tsap_tools  # noqa: E402

MEMBERS = [("dbn", "dbc"), ("akn", "akc")]
TEST_ROOT_KEY = AGENT / "tests" / "fixtures" / "ts-test-root.key"
TEST_ROOT_PUB = AGENT / "tests" / "fixtures" / "ts-test-root.pub"
WASM = REPO / "firmware" / "tests" / "app" / "appw" / "native_app.wasm"
OUT = Path("/tmp/ak6")

MANIFEST = {
    "app_id": "tessera.demo.ln1",
    "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0",
    "caps": ["gpio:write:0-3"],
    "stack_kb": 4,
    "heap_kb": 16,
    "exports": ["health_ping", "app_init", "app_tick", "app_evt"],
}


def log(m: str) -> None:
    print(f"AK6 {m}", flush=True)


def wait_member(svc, node, cube, tag, tries=75):
    ok = 0
    for i in range(tries):
        try:
            rs = svc.query(K.key_sys(node, cube, "get-info"),
                           K.envelope("get-info", f"{tag}-{i}"))
            if rs:
                ok += 1
                if ok >= 2:
                    return True
            else:
                ok = 0
        except Exception:
            ok = 0
        time.sleep(2)
    return False


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    import shutil

    w = OUT / "native_app.wasm"
    shutil.copy(WASM, w)
    pkg = tsap_tools.tsap_package(str(w), MANIFEST, str(TEST_ROOT_KEY),
                                  str(OUT), [str(OUT), str(WASM.parent)])
    log(f"package PASS: {pkg['size']}B")

    reg = LnRegistry()
    with ZenohService("tcp/192.168.2.91:9955") as svc:
        for node, cube in MEMBERS:
            if not wait_member(svc, node, cube, f"hb-{cube}"):
                log(f"FAIL: {cube} 不在线")
                return 1
        log("双板在线（dbc + akc）")

        ln = reg.create("ln-k6", MEMBERS, linkloss_policy="continue")
        log(f"ln_create: {ln.ln_id} members={len(ln.members)}")

        r = ln_deploy(reg, svc, "ln-k6", pkg["package_path"],
                      str(TEST_ROOT_PUB), log_fn=log)
        log(f"ln_deploy 聚合: {r}")

        # 扇出后各板暖复位装载
        time.sleep(12)
        for node, cube in MEMBERS:
            wait_member(svc, node, cube, f"w-{cube}")
        time.sleep(8)
        st = ln_status(reg, svc, "ln-k6", log_fn=log)
        log(f"ln_status 聚合: {st}")

        # 双板终态断言
        ok_all = True
        for node, cube in MEMBERS:
            g = deploy._cmd_ok(svc, node, cube, "get-app", f"fin-{cube}")
            app_id = g.get("app_id", "")
            state = g.get("state", -1)
            log(f"{cube}: state={state} app_id={app_id}")
            if app_id != MANIFEST["app_id"] or state != 3:
                ok_all = False
        if not ok_all:
            log("FAIL: 双板 ACTIVE 断言未过")
            return 1
    print("AK6 LN PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
