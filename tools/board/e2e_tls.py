#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P1-2 TLS E2E 驱动：经双听 zenohd（板 = tls/9956 mTLS；agent = tcp/9957 同路由）
部署 + 判据。板 prov = 15 对 tls/192.168.2.91:9956。"""
import sys
import time
from pathlib import Path

REPO = Path("/home/emb/project/Tessera")
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))
sys.path.insert(0, str(AGENT / "tests"))

from tessera_agent.tools_net import deploy, keys as K  # noqa: E402
from tessera_agent.tools_net.zenoh_service import ZenohService  # noqa: E402
from tessera_agent.tools_tsap import tools as tsap_tools  # noqa: E402
from tessera_agent.tools_tsap.wasm_build import compile_app_c  # noqa: E402

NODE, CUBE = "akn", "akc"
TEST_ROOT_KEY = AGENT / "tests" / "fixtures" / "ts-test-root.key"
TEST_ROOT_PUB = AGENT / "tests" / "fixtures" / "ts-test-root.pub"
APP_SRC = REPO / "agent" / "demos" / "demo-resource-push" / "app.c"
PLAIN = "tcp/192.168.2.91:9957"
MANIFEST = {
    "app_id": "tessera.demo.ak4", "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0", "tier": "large", "caps": ["fs:read:0"],
    "stack_kb": 4, "heap_kb": 16,
    "exports": ["health_ping", "app_init", "app_tick", "app_evt"],
    "fs_paths": ["/SD:/res"],
    "needs": {"linear_memory_kb": 64, "stack_kb": 4, "resources": ["/SD:/res"]},
}


def log(m):
    print(f"TLS-E2E {m}", flush=True)


def wait_online(svc, tries=60):
    for _ in range(tries):
        try:
            rs = svc.query(K.key_sys(NODE, CUBE, "get-info"),
                           K.envelope("get-info", "tls-hb"), 1.5)
            for _, pl in rs:
                if b"akc" in pl:
                    return True
        except Exception:
            pass
        time.sleep(1)
    return False


def main():
    out = Path("/tmp/ak_tls_pkg")
    out.mkdir(exist_ok=True)
    cres = compile_app_c(APP_SRC.read_text(), str(out), [str(out)],
                         required_exports=MANIFEST["exports"],
                         tier="large", needs=MANIFEST["needs"])
    pkg = tsap_tools.tsap_package(str(cres["wasm_path"]), MANIFEST,
                                  str(TEST_ROOT_KEY), str(out), [str(out)])
    log(f"package {pkg['size']}B")
    with ZenohService(PLAIN) as svc:
        if not wait_online(svc):
            log("FAIL: 板不在线（经 9957）")
            return 1
        log("board online（经明文口 9957 → 同路由 TLS 口 9956）")
        d = deploy.push_app(svc, NODE, CUBE, pkg["package_path"],
                            str(TEST_ROOT_PUB), log_fn=log)
        state = d.get("get-app", {}).get("state")
        log(f"deploy result: state={state} pending={d.get('pending_local_confirm')}")
        if d.get("pending_local_confirm"):
            log("PENDING——闸门呈现（60s timeout 路自动 fail-closed——AKP1/AKP4 判据）")
            time.sleep(70)
        # 终态查询
        rs = svc.query(K.key_sys(NODE, CUBE, "get-app"),
                       K.envelope("get-app", "tls-final"), 5.0)
        for _, pl in rs:
            import cbor2
            dd = cbor2.loads(pl).get("data", {})
            log(f"final: state={dd.get('state')} slot={dd.get('active_slot')}")
        log("DONE")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
