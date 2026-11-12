#!/usr/bin/env python3
"""tsap_pack.py 自检（D-2）：独立复刻设备端验签管线（R05 store.c：
TSAP v2 头解析→摘要对拍→COSE_Sign1 精确子集→ed25519 验证），对
打包器产物做字节级验证——不共享 tsap_pack.py 的构造代码路径。"""

import hashlib
import struct
import sys
from pathlib import Path

from nacl.signing import VerifyKey


def bstr_size(n: int) -> int:
    return 1 if n < 24 else 2 if n < 256 else 3 if n < 65536 else 5


def verify(tsap: bytes, seed_and_pk: bytes) -> None:
    assert tsap[:4] == b"TSAP", "magic"
    (ver,) = struct.unpack(">H", tsap[4:6])
    mlen, wlen = struct.unpack(">II", tsap[6:14])
    (flags,) = struct.pack(">H", 0)[:0] or struct.unpack(">H", tsap[14:16])
    assert ver == 2 and flags == 1, "fmt_ver/flags"
    dg = tsap[16:48]
    content = tsap[48:]
    manifest, rest = content[:mlen], content[mlen:]
    wasm, cose = rest[:wlen], rest[wlen:]
    assert hashlib.sha256(manifest + wasm).digest() == dg, "digest mismatch"
    # COSE_Sign1 精确子集
    assert cose[0:2] == b"\xd2\x84", "tag18+array4"
    assert cose[2:6] == b"\x43\xa1\x01\x27", "phdr {1:-8}"
    assert cose[6] == 0xA0, "uhdr map0"
    plen_off = 7
    # payload bstr 头（canonical）
    pl0 = cose[plen_off]
    if pl0 < 0x58:
        plen, hdr_len = pl0 & 0x1F, 1
    elif pl0 == 0x58:
        plen, hdr_len = cose[8], 2
    elif pl0 == 0x59:
        plen, hdr_len = struct.unpack(">H", cose[8:10])[0], 3
    else:
        plen, hdr_len = struct.unpack(">I", cose[8:12])[0], 5
    payload = cose[7 + hdr_len : 7 + hdr_len + plen]
    assert payload == manifest + wasm, "内嵌 payload 副本 == 裸段（签A装B防御）"
    sig_off = 7 + hdr_len + plen
    assert cose[sig_off : sig_off + 2] == b"\x58\x40", "sig bstr(64)"
    sig = cose[sig_off + 2 : sig_off + 66]
    assert len(sig) == 64, "sig len"
    # Sig_structure 重建（设备同形）并验签
    uhdr_bstr = b"\x40"  # 设备管线：外层 array 元素 uhdr=bstr(0)
    sig_struct = (
        b"\x84\x6aSignature1\x43\xa1\x01\x27"
        + uhdr_bstr
        + cose[7 : 7 + hdr_len]
        + payload
    )
    pk = seed_and_pk[32:]
    VerifyKey(pk).verify(sig_struct, sig)
    print(
        f"OK: {len(tsap)}B tsap verified (manifest {mlen}B + wasm {wlen}B "
        f"+ cose {len(cose)}B; digest {dg[:8].hex()}…)"
    )


if __name__ == "__main__":
    tsap = Path(sys.argv[1]).read_bytes()
    # tweetnacl 64B 形（seed‖pk）：从 .inc 提取或由 seed 推导——
    # 此处从 fixtures .pub（32B pk）+ 打包用 seed 组合验证。
    pub = Path(sys.argv[2]).read_bytes()
    seed = Path(sys.argv[3]).read_bytes()
    verify(tsap, seed + pub)
