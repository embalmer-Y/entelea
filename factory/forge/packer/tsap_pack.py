#!/usr/bin/env python3
"""Forge TSAP v2 打包签名器（D-2；R05/R12 工厂契约）。

字节格式与设备端验签管线（framework/services/package/store.c 及
platform/zephyr/tests/pkg 对照实现）逐字节对齐：

  头16B: "TSAP" | fmt_ver u16BE(=2) | manifest_len u32BE | wasm_len u32BE
         | flags u16BE(=1 digest)
  摘要32B: sha256(manifest ‖ wasm)
  内容:   manifest(CBOR) ‖ wasm ‖ COSE_Sign1(ed25519)
  COSE_Sign1 精确子集: tag18 + array4 ["Signature1"(0x6a),
         phdr bstr{1:-8}(0x43 a1 01 27), uhdr map0(0x40→a0),
         payload bstr(内嵌 manifest‖wasm 副本), sig bstr64]

签名实现=pynacl/libsodium（与设备 tweetnacl 路径独立互验——D-4 对拍
的价值前提）。密钥=公开测试根（旧树 fixtures，Q-29④：测试信任≠生产
信任）。产出的 candidate.json 域校验与固件 entelea_factory_intake
同域（fail-closed 提前拒）。
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import nacl.signing


def cbor_hdr_bstr(n: int) -> bytes:
    """CBOR bstr 头（canonical：24/256/65536 阈值）。"""
    if n < 24:
        return bytes([0x40 | n])
    if n < 256:
        return bytes([0x58, n])
    if n < 65536:
        return bytes([0x59]) + struct.pack(">H", n)
    return bytes([0x5A]) + struct.pack(">I", n)


def cbor_uint(n: int) -> bytes:
    """CBOR 无符号整数（RFC 8949 首选序列化：最短编码）。"""
    if not 0 <= n <= 0xFFFFFFFF:
        raise ValueError("cbor_uint out of range")
    if n < 24:
        return bytes([n])
    if n < 256:
        return bytes([0x18, n])
    if n < 65536:
        return b"\x19" + struct.pack(">H", n)
    return b"\x1A" + struct.pack(">I", n)


def build_manifest(version: int, mem_kb: int, steps: int, fmt: str) -> bytes:
    """工厂产物描述（R05 合同：map{version,mem_kb,steps,fmt} 确定性编码）。"""
    fmt_id = {"wasm": 0, "lua": 1}[fmt]
    # canonical CBOR map(4)：键按序 0..3（int 键 canonical 序=数值序），
    # 值为 RFC 8949 首选（最短）整数编码——与设备 entelea_lua_contract_encode
    # 逐字节一致（RF1：此前 steps<24 误用 0x18 两字节形式）。
    out = bytearray([0xA4])
    for key, val in enumerate((version, mem_kb, steps, fmt_id)):
        out += bytes([key]) + cbor_uint(val)
    return bytes(out)


def sig_structure(payload: bytes) -> bytes:
    """COSE Sig_structure（无签名的被签文）：["Signature1",phdr,{},payload]。"""
    return (b"\x84" b"\x6a" b"Signature1"
            b"\x43\xa1\x01\x27"   # phdr bstr(3) = {1:-8} EdDSA
            b"\x40"               # uhdr bstr(0)（外层为 map 时 a0——见下）
            + cbor_hdr_bstr(len(payload)) + payload)


def build_tsap(manifest: bytes, wasm: bytes, seed: bytes) -> bytes:
    payload = manifest + wasm
    digest = hashlib.sha256(payload).digest()
    sk = nacl.signing.SigningKey(seed)
    sig = sk.sign(sig_structure(payload)).signature
    assert len(sig) == 64

    cose = (b"\xd2\x84"                     # tag18 + array(4)
            b"\x43\xa1\x01\x27"             # phdr bstr(3)
            b"\xa0"                         # uhdr map(0)
            + cbor_hdr_bstr(len(payload)) + payload
            + b"\x58\x40" + sig)            # sig bstr(64)
    hdr = (b"TSAP" + struct.pack(">H", 2)
           + struct.pack(">I", len(manifest)) + struct.pack(">I", len(wasm))
           + struct.pack(">H", 1))
    assert len(hdr) == 16
    return hdr + digest + payload + cose


def emit_c_header(tsap: bytes, cand: dict, path: Path) -> None:
    """C include（设备闸构建期注入——交付面=USB 烧录路径）：
    TSAP 字节数组 + candidate 元数据（与固件 entelea_factory_candidate_t
    对齐；description 以 NUL 终止字节数组交付）。"""
    dg = bytes.fromhex(cand["package_digest"])
    desc = cand["description"].encode("utf-8")[:47]
    with path.open("w") as f:
        f.write("/* Forge 工厂产物（生成：tsap_pack.py；勿手改） */\n")
        f.write("static const uint8_t FACTORY_TSAP[] = {\n")
        for i in range(0, len(tsap), 12):
            row = ",".join(f"0x{b:02x}" for b in tsap[i:i + 12])
            f.write(f"\t{row},\n")
        f.write("};\n")
        f.write(f"#define FACTORY_TSAP_LEN {len(tsap)}u\n")
        f.write(f"static const uint8_t FACTORY_DIGEST[32] = {{\n\t"
                + ",".join(f"0x{b:02x}" for b in dg) + "\n};\n")
        f.write(f"#define FACTORY_PERMISSIONS {cand['permissions']}u\n")
        f.write(f"#define FACTORY_VERSION {cand['runtime_version']}u\n")
        f.write(f"#define FACTORY_KIND {cand['payload_kind']}u\n")
        f.write(f"#define FACTORY_BUDGET {cand['budget_steps']}u\n")
        f.write(f"#define FACTORY_MEM_KB {cand['mem_quota_kb']}u\n")
        f.write('static const char FACTORY_DESC[] = "'
                + desc.decode("latin1").replace("\\", "\\\\")
                  .replace('"', '\\"')
                + '";\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wasm", type=Path, required=True)
    ap.add_argument("--key", type=Path, required=True,
                    help="32B ed25519 seed（公开测试根）")
    ap.add_argument("--version", type=int, required=True)
    ap.add_argument("--mem-kb", type=int, required=True)
    ap.add_argument("--steps", type=int, required=True)
    ap.add_argument("--fmt", choices=["wasm", "lua"], default="wasm")
    ap.add_argument("--permissions", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--description", default="")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    # 域校验（与 entelea_factory_intake 同域 fail-closed）
    if not (0 < args.steps <= 10_000_000) or not (0 < args.mem_kb <= 512):
        print("FAIL: 预算域外（0<budget≤1e7、0<mem≤512KB）", file=sys.stderr)
        return 1
    if not (0 <= args.permissions <= 0xFFFF) or not (0 <= args.version <= 0xFF):
        print("FAIL: permissions/version 域外", file=sys.stderr)
        return 1
    if len(args.description) > 48:
        print("FAIL: description >48 字符", file=sys.stderr)
        return 1
    seed = args.key.read_bytes()
    if len(seed) != 32:
        print("FAIL: key 必须 32B seed", file=sys.stderr)
        return 1
    wasm = args.wasm.read_bytes()
    if len(wasm) > 64 * 1024:
        print("FAIL: wasm 超 64KB 装载上限", file=sys.stderr)
        return 1

    manifest = build_manifest(args.version, args.mem_kb, args.steps, args.fmt)
    tsap = build_tsap(manifest, wasm, seed)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "app.tsap").write_bytes(tsap)
    cand = {
        "package_digest": hashlib.sha256(tsap).hexdigest(),
        "permissions": args.permissions,
        "runtime_version": args.version,
        "payload_kind": 0 if args.fmt == "wasm" else 1,
        "budget_steps": args.steps,
        "mem_quota_kb": args.mem_kb,
        "description": args.description,
        "tsap_len": len(tsap),
    }
    (args.out / "candidate.json").write_text(
        json.dumps(cand, indent=2, ensure_ascii=False) + "\n")
    emit_c_header(tsap, cand, args.out / "factory_pkg.h")
    print(f"OK: {args.out}/app.tsap ({len(tsap)}B) digest={cand['package_digest'][:16]}…")
    return 0


if __name__ == "__main__":
    sys.exit(main())
