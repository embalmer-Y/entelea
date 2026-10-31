#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P4-W2 探针包生成器（V0.14.0 · tools/board 惯例：机械生成 + 测试根签名）。

产物（全部入仓可复现）：
  agent/demos/demo-p4-probe/test-root.pub      —— 探针根公钥（32B raw；key 本体留 gitignored agent/keys/）
  agent/demos/demo-p4-probe/p4w.probe-1.0.0.tsap —— 探针包（log-only caps，编译级确定性）

用法：agent venv `python tools/board/gen_p4_probe.py`（需 clang wasm32 + 仓内工具链面）。
"""
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEMO = REPO / "agent" / "demos" / "demo-p4-probe"


def main() -> int:
    sys.path.insert(0, str(REPO / "agent"))

    from tessera_agent.tools_tsap.tools import tsap_package, tsap_verify
    from tessera_agent.tools_tsap.wasm_build import compile_app_c

    # 测试根 = 仓内 fixture（固件 TEST 构建的工厂根 = 编译内固定测试根
    # verify.c test_root_pub——Q-29④ 测试专用公开材料；自造新钥会被
    # TEST 板如实拒〔P4-W2 实测：板侧 pk0=f084a4e5 = fixture 根〕）
    key = REPO / "agent" / "tests" / "fixtures" / "ts-test-root.key"
    pub = REPO / "agent" / "tests" / "fixtures" / "ts-test-root.pub"
    (DEMO / "test-root.pub").write_bytes(pub.read_bytes())
    print(f"pub: {DEMO / 'test-root.pub'}（{len(pub.read_bytes())}B）")

    out_dir = DEMO / "build"
    out_dir.mkdir(parents=True, exist_ok=True)
    compiled = compile_app_c(
        (DEMO / "app.c").read_text(encoding="utf-8"),
        str(out_dir), [str(REPO)],
        required_exports=["health_ping", "app_init", "app_tick", "app_evt"],
        threads=1, tier="small")
    print(f"compile: {compiled['size']}B imports={compiled['imports']}")

    manifest = json.loads((DEMO / "manifest.json").read_text(encoding="utf-8"))
    packaged = tsap_package(
        compiled["wasm_path"], manifest, str(key), str(out_dir),
        allowed_roots=[str(REPO)])
    tsap_verify(packaged["package_path"], str(DEMO / "test-root.pub"),
                allowed_roots=[str(REPO)])
    final = DEMO / "p4w.probe-1.0.0.tsap"
    final.write_bytes(Path(packaged["package_path"]).read_bytes())
    print(f"package: {final}（{final.stat().st_size}B，verify PASS）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
