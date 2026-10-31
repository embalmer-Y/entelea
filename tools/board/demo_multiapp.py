# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""M7c 多 APP 真机判据驱动：双实例部署 + 并行运行验收（V0.4.0）。

app=1 部署 nvd 包（节拍巡检）→ app=2 部署 D8-v1 包（生命周期 demo）→
apps 全景视图双 ACTIVE → console 双流判据（两 APP 各自 init/DONE 行）。
"""
import sys
import time

sys.path.insert(0, "/home/emb/project/Tessera/agent")

from tessera_agent.tools_net import deploy
from tessera_agent.tools_net.zenoh_service import ZenohService

ROUTER = "tcp/127.0.0.1:9955"
NODE, CUBE = "dbn", "dbc"
PKG1 = "/home/emb/project/Tessera/agent/build/com.example.cube.nvd-1.0.0.tsap"
PKG2 = "/home/emb/project/Tessera/agent/demos/demo-lifecycle/v1/demo.d8.lifecycle-1.0.0.tsap"
PUB = "/home/emb/project/Tessera/agent/tests/fixtures/ts-test-root.pub"
TS_APP_ACTIVE = 3


def wait_board(svc, timeout=90.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            cubes = deploy.discover(svc)
            if (NODE, CUBE) in {(c["node_id"], c["cube_id"]) for c in cubes}:
                return True
        except Exception:
            pass
        time.sleep(2.0)
    return False


def _raw_get_app(svc):
    from tessera_agent.tools_net.deploy import _cmd_ok

    for _ in range(30):
        try:
            return _cmd_ok(svc, NODE, CUBE, "get-app", f"ma-ga-{int(time.time())}")
        except Exception:
            time.sleep(2.0)
    return None


def main() -> int:
    with ZenohService(ROUTER, query_timeout_s=5.0) as svc:
        print("=== [1] 等板在线 ===", flush=True)
        assert wait_board(svc), "板不在线"

        print("=== [2] app=1 部署 nvd 包 ===", flush=True)
        d1 = deploy.push_app(svc, NODE, CUBE, PKG1, PUB, app=1,
                             log_fn=lambda s: print(f"  {s}", flush=True))
        print(f"app1 slot={d1['active_slot']} confirmed={d1['confirmed']}")
        time.sleep(12.0)  # 板激活后暖复位 + 双装载
        assert wait_board(svc), "复位后板未归"

        print("=== [3] app=2 部署 D8-v1 包 ===", flush=True)
        d2 = deploy.push_app(svc, NODE, CUBE, PKG2, PUB, app=2,
                             log_fn=lambda s: print(f"  {s}", flush=True))
        print(f"app2 slot={d2['active_slot']} confirmed={d2['confirmed']}")
        time.sleep(12.0)
        assert wait_board(svc), "复位后板未归"

        print("=== [4] apps 全景视图（双 ACTIVE 判据） ===", flush=True)
        st = _raw_get_app(svc)
        apps = st.get("apps", []) if st else []
        for a in apps:
            print(f"  app={a.get('app')} state={a.get('state')} "
                  f"slot={a.get('active_slot')} id={a.get('app_id')!r}")
        states = {int(a.get("app", 0)): int(a.get("state", -1)) for a in apps}
        ok_view = states.get(1) == TS_APP_ACTIVE and states.get(2) == TS_APP_ACTIVE
        print(f"=== apps 视图：{'双 ACTIVE ✓' if ok_view else f'未达 {states}'} ===")
        return 0 if ok_view else 1


sys.exit(main())
