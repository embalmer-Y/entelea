#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""e2e_resource_push.py — K4 net+SD 资源推送真机 E2E 驱动（仓外惯例）。

链路：编译 demo-resource-push（原 D-AK4）app → 测试根签名打包（needs.resources）→ push_app 激活
→ 暖复位装载 → push_resources（sha256 verified）→ console AK4 DONE。
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
APP_SRC = REPO / "agent" / "demos" / "demo-resource-push" / "app.c"
CONSOLE = Path("/tmp/ak3_console_live.txt")
OUT = Path("/tmp/ak4")

# 与 app.c EXPECT 逐字节一致
RES_BYTES = b"AK4-RES-" + bytes(ord("0") + (i % 10) for i in range(28))
assert len(RES_BYTES) == 36

# K5 联动：needs 仅 large 档可携（固件 pkg.c schema 纪律）——资源推送授权
# 面随 large 包；needs.linear_memory_kb/stack_kb 供链接期钉死内存（CG4）。
MANIFEST = {
    "app_id": "tessera.demo.ak4",
    "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0",
    "tier": "large",
    "caps": ["fs:read:0"],
    "stack_kb": 4,
    "heap_kb": 16,
    "exports": ["health_ping", "app_init", "app_tick", "app_evt"],
    "fs_paths": ["/SD:/res"],
    "needs": {"linear_memory_kb": 64, "stack_kb": 4,
              "resources": ["/SD:/res"]},
}


def log(msg: str) -> None:
    print(f"AK4 {msg}", flush=True)


def wait_online(svc, tag: str, tries: int = 75) -> bool:
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


def console_has(marker: str, since_line: int) -> bool:
    try:
        lines = CONSOLE.read_text(errors="replace").splitlines()
    except OSError:
        return False
    return any(marker in ln for ln in lines[since_line:])


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)

    # 1) 编译（真 clang 链：白名单面检查 + 尺寸门 + 双编译一致）
    cres = compile_app_c(APP_SRC.read_text(), str(OUT), [str(OUT)],
                         required_exports=MANIFEST["exports"],
                         tier="large", needs=MANIFEST["needs"])
    wasm = Path(cres["wasm_path"])
    log(f"compile PASS: {cres['size']}B imports={cres['imports']}")

    # 2) 打包（测试根签名）
    pkg = tsap_tools.tsap_package(str(wasm), MANIFEST, str(TEST_ROOT_KEY),
                                  str(OUT), [str(OUT), str(wasm.parent)])
    log(f"package PASS: {pkg['size']}B")

    with ZenohService("tcp/192.168.2.91:9955") as svc:
        if not wait_online(svc, "hb"):
            log("FAIL: 板不在线")
            return 1
        log("board online")
        # 3) 部署 + 激活（暖复位装载）
        console_lines = len(CONSOLE.read_text(errors="replace").splitlines()) \
            if CONSOLE.exists() else 0
        d = deploy.push_app(svc, NODE, CUBE, pkg["package_path"],
                            str(TEST_ROOT_PUB), log_fn=log)
        assert d.get("confirmed") is True
        log(f"push_app PASS slot={d.get('active_slot')}")
        time.sleep(10)
        if not wait_online(svc, "w"):
            log("FAIL: 复位后板未回线")
            return 1
        # 4) 资源推送（CG5-B：sha256 读回校验 verified）
        r = deploy.push_resources(svc, NODE, CUBE,
                                  {"/SD:/res/ak4.txt": RES_BYTES},
                                  log_fn=log)
        verified = r.get("verified") or r.get("files", {})
        log(f"push_resources: {r}")
        # 5) 板侧消费判据（console 轮询 90s）
        for _ in range(45):
            if console_has("ak4 match=1", console_lines) and \
               console_has("AK4 DONE", console_lines):
                log("板侧消费命中：ak4 match=1 + AK4 DONE")
                print("AK4 RES PASS")
                return 0
            if console_has("AK4 FAIL", console_lines):
                log("FAIL: 板侧 40 tick 未命中")
                return 1
            time.sleep(2)
        log("FAIL: 等待板侧判据超时")
        return 1


if __name__ == "__main__":
    sys.exit(main())
