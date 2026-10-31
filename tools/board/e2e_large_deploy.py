#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""e2e_large_deploy.py — K5 large 档全链验收（真 ~200KB wasm）。

编译（large 门 256KB）→ 打包（tier=large + needs）→ 验签 → 安装 → 激活
→ 暖复位 PSRAM 装载 → 资源推送 → 板侧 AK4 DONE（大包消费端同判据）。
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

REPO = Path("/home/emb/project/Tessera")
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_net import keys as K  # noqa: E402
from tessera_agent.tools_net import deploy  # noqa: E402
from tessera_agent.tools_net.zenoh_service import ZenohService  # noqa: E402
from tessera_agent.tools_tsap import tools as tsap_tools  # noqa: E402
from tessera_agent.tools_tsap.wasm_build import compile_app_c  # noqa: E402

NODE, CUBE = "akn", "akc"
TEST_ROOT_KEY = AGENT / "tests" / "fixtures" / "ts-test-root.key"
TEST_ROOT_PUB = AGENT / "tests" / "fixtures" / "ts-test-root.pub"
CONSOLE = Path("/tmp/ak3_console_live.txt")
OUT = Path("/tmp/ak4")

RES_BYTES = b"AK4-RES-" + bytes(ord("0") + (i % 10) for i in range(28))

MANIFEST = {
    "app_id": "tessera.demo.ak4big",
    "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0",
    "tier": "large",
    "caps": ["fs:read:0"],
    "stack_kb": 4,
    "heap_kb": 16,
    "exports": ["health_ping", "app_init", "app_tick", "app_evt"],
    "fs_paths": ["/SD:/res"],
    "needs": {"linear_memory_kb": 256, "stack_kb": 4,
              "resources": ["/SD:/res"]},
}


def log(m: str) -> None:
    print(f"AK5 {m}", flush=True)


def wait_online(svc, tag, tries=75):
    ok = 0
    for i in range(tries):
        try:
            rs = svc.query(K.key_sys(NODE, CUBE, "get-info"),
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
    cres = compile_app_c(Path("/tmp/ak4/app_big.c").read_text(),
                         str(OUT), [str(OUT)],
                         required_exports=MANIFEST["exports"],
                         tier="large", needs=MANIFEST["needs"])
    log(f"large compile PASS: {cres['size']}B")
    if cres["size"] < 150 * 1024:
        log(f"FAIL: 载荷未达 large 量级（{cres['size']}B）")
        return 1
    pkg = tsap_tools.tsap_package(cres["wasm_path"], MANIFEST,
                                  str(TEST_ROOT_KEY), str(OUT),
                                  [str(OUT)])
    log(f"package PASS: {pkg['size']}B")
    with ZenohService("tcp/192.168.2.91:9955") as svc:
        if not wait_online(svc, "hb"):
            log("FAIL: 板不在线")
            return 1
        lines0 = len(CONSOLE.read_text(errors="replace").splitlines()) \
            if CONSOLE.exists() else 0
        d = deploy.push_app(svc, NODE, CUBE, pkg["package_path"],
                            str(TEST_ROOT_PUB), log_fn=log)
        assert d.get("confirmed") is True
        log(f"push_app PASS slot={d.get('active_slot')}")
        time.sleep(10)
        if not wait_online(svc, "w"):
            log("FAIL: 复位后板未回线")
            return 1
        deploy.push_resources(svc, NODE, CUBE,
                              {"/SD:/res/ak4.txt": RES_BYTES}, log_fn=log)
        for _ in range(45):
            txt = CONSOLE.read_text(errors="replace") if CONSOLE.exists() else ""
            ls = txt.splitlines()
            if any("ak4 match=1" in ln for ln in ls[lines0:]) and \
               any("AK4 DONE" in ln for ln in ls[lines0:]):
                log(f"大包全链命中（wasm {cres['size']}B PSRAM 装载 + 运行）")
                print("AK5 LARGE PASS")
                return 0
            if any("AK4 FAIL" in ln for ln in ls[lines0:]):
                log("FAIL: 板侧 40 tick 未命中")
                return 1
            time.sleep(2)
        log("FAIL: 判据超时")
        return 1


if __name__ == "__main__":
    sys.exit(main())
