#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""gen_tier_pkgs.py — CG4 档位夹具生成器（机械生成纪律，教训 27）。

用 Agent 打包链（tools_tsap.tsap_package + 固件固定测试根签名——
agent/tests/fixtures/ts-test-root.key，Q-29④ 公开测试材料）生成两类
TSAP v2 预签包 C 头：
  - boot_pkg_large.h：tier=large + needs{linear_memory_kb=64, stack_kb=16,
    resources=["/SD:/res"]}（wasm 同 native_app.wasm 夹具——native_sim =
    small-only 设备：安装期匹配矩阵拒装用例的载荷）
再生成一版 tier=small 携 needs（schema 违例样例，扫描矩阵用——
walk/scan fail-closed 断言）。

用法：~/project/agent-venv/bin/python gen_tier_pkgs.py（仓内自足——
脚本随仓，产物头随仓；重跑 = 复现字节）。
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_tsap.tools import tsap_package  # noqa: E402

APPW = Path(__file__).resolve().parent / "appw"
KEY = AGENT / "tests" / "fixtures" / "ts-test-root.key"
OUT = Path("/tmp/tier-pkgs")

# 差异化 wasm：native_app.c + pad 全局（语义同源；字节差异 → WAMR 模块
# 缓存 miss——进程内二次 boot_start 的同内容怪癖〔dev-env 教训 28〕绕开，
# test_04 已用原 wasm boot 过一次）。禁手抄——本脚本机械构建。
# pad 经 health_ping 引用（volatile 读，&0 掩码 = 零行为语义差）——防
# 链接期 GC（未引用全局实测被剥离，字节与原 wasm 全同）。
_ORIG_PING = '''__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return g_health;
}'''
_PAD_PING = '''volatile int tier_fixture_pad = 7; /* CG4 夹具 pad（仅差异字节） */

__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return g_health + (tier_fixture_pad & 0);
}'''
VARIANT_SRC = (APPW / "native_app.c").read_text(encoding="utf-8").replace(
    _ORIG_PING, _PAD_PING)
assert _PAD_PING.split("\n")[0] in VARIANT_SRC and "tier_fixture_pad" in VARIANT_SRC
CLANG_CMD = [
    "clang", "--target=wasm32", "-O2", "-nostdlib", "-ffreestanding",
    "-fno-builtin", "-Wl,--no-entry", "-Wl,--allow-undefined",
    "-Wl,--strip-all",
]
# pad 全局改变数据段字节 → 缓存 miss；wasm 无多余导出（与 native_app 夹具
# 同形——固件 walk 只消费 manifest 导出表）。

BASE_MANIFEST = {
    "app_id": "com.tessera.fixture.tier",
    "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0",
    "caps": ["gpio:write:0-3"],
    "stack_kb": 8,
    "heap_kb": 4,
    "exports": ["health_ping"],
}

LARGE_MANIFEST = {
    **BASE_MANIFEST,
    "tier": "large",
    "needs": {"linear_memory_kb": 64, "stack_kb": 16,
              "resources": ["/SD:/res"]},
}


def emit_header(name: str, data: bytes, comment: str) -> None:
    lines = ["/* SPDX-License-Identifier: Apache-2.0 */",
             f"/* {comment} */",
             f"/* 机械生成（gen_tier_pkgs.py——重跑复现；禁手抄，教训 27） */",
             f"#ifndef {name.upper()}_H__", f"#define {name.upper()}_H__", "",
             f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 12):
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
    lines += ["};", "", "#endif", ""]
    (Path(__file__).resolve().parent / "src" / f"{name}.h").write_text(
        "\n".join(lines), encoding="utf-8")
    print(f"{name}.h: {len(data)}B")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    src = OUT / "native_app_tier.c"
    wasm = OUT / "native_app_tier.wasm"
    src.write_text(VARIANT_SRC, encoding="utf-8")
    subprocess.run([*CLANG_CMD, "-o", str(wasm), str(src)], check=True)
    print(f"variant wasm: {wasm.stat().st_size}B")
    r = tsap_package(str(wasm), LARGE_MANIFEST, str(KEY), str(OUT),
                     [str(OUT)])
    emit_header("boot_pkg_large", Path(r["package_path"]).read_bytes(),
                "CG4 档位夹具：tier=large + needs + 差异化 wasm（native_sim small-only → 安装期匹配拒装；boot 直装走查用）")
    print(f"package: {r['package_path']} ({r['size']}B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
