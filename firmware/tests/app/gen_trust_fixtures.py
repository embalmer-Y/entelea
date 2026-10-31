#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""gen_trust_fixtures.py — EC2 信任链夹具生成器（机械生成纪律，教训 27）。

产物 src/trust_fixtures.h（固件 framework.app test_13 消费）：
  - trust_dev_pub：开发者键公钥（固定种子派生——重跑复现字节）
  - trust_blob_add：COSE_Sign1(表{ver:1, keys:[dev]}) 签名 = 测试根
    （首次注册由工厂根授权——Q-43④）
  - trust_blob_dev_signed：ver:2 keys:[dev] 签名 = 开发者键（有效集内
    互荐——次版由现信任键推动）
  - trust_blob_clear：ver:3 keys:[] 签名 = 测试根（全撤回工厂根单源）
  - trust_blob_replay：ver:1 重放样例（已应用版本 → E_RANGE）
  - trust_blob_skip：ver:9 跳版样例（→ E_RANGE）
  - trust_blob_unauth：ver:4 签名 = 陌生键（有效集外 → E_INVALID_SIG）
  - boot_pkg_devroot：差异化 wasm 的 TSAP 包，签名 = 开发者键
    （verify_slot NULL 多根实证载荷）

表 CBOR = 固件定稿键序 map{"ver":uint,"keys":[bstr(32)×n]}（2 对定序——
与 ts_trust.c trust_walk 字节级同构；手工构造非 cbor2 canonical）。

用法：~/project/agent-venv/bin/python gen_trust_fixtures.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

REPO = Path(__file__).resolve().parents[3]
AGENT = REPO / "agent"
sys.path.insert(0, str(AGENT))

from tessera_agent.tools_tsap.cose import sign_diy  # noqa: E402
from tessera_agent.tools_tsap.tools import tsap_package  # noqa: E402

APPW = Path(__file__).resolve().parent / "appw"
ROOT_KEY = (AGENT / "tests" / "fixtures" / "ts-test-root.key").read_bytes()
OUT = Path("/tmp/trust-fixtures")

# 开发者键：固定种子派生（重跑复现；测试材料）
DEV_SEED = bytes(range(1, 33))
_dev_priv_obj = Ed25519PrivateKey.from_private_bytes(DEV_SEED)
DEV_PRIV = DEV_SEED
DEV_PUB = _dev_priv_obj.public_key().public_bytes_raw()

# 陌生键（有效集外签名样例）
STRANGER_SEED = bytes(range(33, 65))
STRANGER_PRIV = STRANGER_SEED


def _tstr(s: str) -> bytes:
    b = s.encode()
    return bytes([0x60 | len(b)]) + b


def _uint(v: int) -> bytes:
    assert v < 24
    return bytes([v])


def table_cbor(ver: int, keys: list[bytes]) -> bytes:
    out = bytes([0xA2]) + _tstr("ver") + _uint(ver) + _tstr("keys")
    out += bytes([0x80 | len(keys)])
    for k in keys:
        out += bytes([0x58, len(k)]) + k
    return out


def emit(name: str, data: bytes, comment: str) -> None:
    lines = ["/* SPDX-License-Identifier: Apache-2.0 */",
             f"/* {comment} */",
             f"/* 机械生成（gen_trust_fixtures.py——重跑复现；禁手抄，教训 27） */",
             f"#ifndef {name.upper()}_H__", f"#define {name.upper()}_H__", ""]
    if name.endswith("_pub"):
        lines.append(f"static const uint8_t {name}[{len(data)}] = {{")
    else:
        lines.append(f"static const uint8_t {name}[{len(data)}] = {{")
    for i in range(0, len(data), 12):
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
    lines += ["};", "", "#endif", ""]
    (Path(__file__).resolve().parent / "src" / f"{name}.h").write_text(
        "\n".join(lines), encoding="utf-8")
    print(f"{name}.h: {len(data)}B")


# 差异化 wasm（模块缓存怪癖绕开——gen_tier_pkgs 同型纪律；本夹具再差异一层）
_ORIG_PING = '''__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return g_health;
}'''
_TRUST_PING = '''volatile int trust_fixture_pad = 9; /* EC2 夹具 pad（仅差异字节） */

__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return g_health + (trust_fixture_pad & 0);
}'''

DEV_MANIFEST = {
    "app_id": "com.tessera.fixture.devroot",
    "app_ver": "1.0.0",
    "min_fw_ver": "0.1.0",
    "caps": ["gpio:write:0-3"],
    "stack_kb": 8,
    "heap_kb": 4,
    "exports": ["health_ping"],
}


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)

    # ① 更新 blob 族（签名键即授权语义——Q-43④）
    emit("trust_dev_pub", DEV_PUB, "EC2 夹具：开发者公钥（固定种子派生）")
    emit("trust_blob_add", sign_diy(table_cbor(1, [DEV_PUB]), ROOT_KEY),
         "EC2 夹具：ver=1 keys=[dev] 签名=测试根（首次注册）")
    emit("trust_blob_dev_signed", sign_diy(table_cbor(2, [DEV_PUB]), DEV_PRIV),
         "EC2 夹具：ver=2 keys=[dev] 签名=开发者键（有效集内互荐）")
    emit("trust_blob_clear", sign_diy(table_cbor(3, []), ROOT_KEY),
         "EC2 夹具：ver=3 keys=[] 签名=测试根（全撤回工厂根单源）")
    emit("trust_blob_replay", sign_diy(table_cbor(1, [DEV_PUB]), ROOT_KEY),
         "EC2 夹具：ver=1 重放样例（已应用 → E_RANGE）")
    emit("trust_blob_skip", sign_diy(table_cbor(9, [DEV_PUB]), ROOT_KEY),
         "EC2 夹具：ver=9 跳版样例（→ E_RANGE）")
    emit("trust_blob_unauth", sign_diy(table_cbor(1, [DEV_PUB]), STRANGER_PRIV),
         "EC2 夹具：ver=1 签名=陌生键（ver 门过/签名拒 → E_INVALID_SIG；置于 add 之前消费）")

    # ② 开发者键签名 TSAP 包（多根验签载荷——差异化 wasm 防模块缓存同内容怪癖）
    src = (APPW / "native_app.c").read_text(encoding="utf-8")
    variant = src.replace(_ORIG_PING, _TRUST_PING)
    assert _TRUST_PING.split("\n")[0] in variant and "trust_fixture_pad" in variant
    csrc = OUT / "native_app_trust.c"
    wasm = OUT / "native_app_trust.wasm"
    csrc.write_text(variant, encoding="utf-8")
    subprocess.run(
        ["clang", "--target=wasm32", "-O2", "-nostdlib", "-ffreestanding",
         "-fno-builtin", "-Wl,--no-entry", "-Wl,--allow-undefined",
         "-Wl,--strip-all", "-o", str(wasm), str(csrc)], check=True)
    dev_key_file = OUT / "dev.key"
    dev_key_file.write_bytes(DEV_PRIV)
    r = tsap_package(str(wasm), DEV_MANIFEST, str(dev_key_file), str(OUT),
                     [str(OUT)])
    emit("boot_pkg_devroot", Path(r["package_path"]).read_bytes(),
         "EC2 夹具：开发者键签名 TSAP 包（verify_slot NULL 多根实证）")
    print(f"dev package: {r['package_path']} ({r['size']}B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
