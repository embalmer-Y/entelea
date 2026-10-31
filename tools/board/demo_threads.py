# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""demo-threads 全链演示驱动（原 D-THR）（V2 纵深批 M19b，DEC-55/Q-37 A）：
spec → 真 LLM app_develop（线程档编译 + TSAP 打包）→ 提取 wasm →
dt_wasm.h 机械生成（thrdemo 嵌入）。烧录/判据由外部编排。

用法（agent-venv python，需先 source ~/project/logs/env.sh 提供 ANTHROPIC_*）。
"""
import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, "/home/emb/project/Tessera/agent")

from tessera_agent.common.config import load_config
from tessera_agent.common.limits import ContextBudget
from tessera_agent.platform import AppContext
from tessera_agent.domain.app_chain import app_develop

KEY = "/home/emb/project/Tessera/agent/tests/fixtures/ts-test-root.key"
OUT_DIR = "/home/emb/project/Tessera/agent/build"
SPEC_PATH = Path("/home/emb/project/Tessera/agent/demos/demo-threads/spec.md")
PKG_HDR = Path("/home/emb/project/Tessera/firmware/tests/thrdemo/src/dt_wasm.h")
DEMO_DIR = Path("/home/emb/project/Tessera/agent/demos/demo-threads")


def tsap_extract_wasm(tsap: Path) -> bytes:
    """TSAP v2：16B 头（magic4|ver2|manifest_len4|wasm_len4|flags2）+
    32B 摘要 + manifest(CBOR) + wasm（tsap.h 布局）。"""
    blob = tsap.read_bytes()
    assert blob[:4] == b"TSAP"
    mlen = int.from_bytes(blob[6:10], "big")
    wlen = int.from_bytes(blob[10:14], "big")
    wasm_off = 48 + mlen
    return blob[wasm_off:wasm_off + wlen]


def gen_wasm_header(wasm: bytes, out: Path) -> None:
    lines = [f"/* 机械生成（demo_threads.py）——demo-threads app.wasm，{len(wasm)}B */",
             f"static uint8_t dt_wasm[{len(wasm)}] = {{"]
    for i in range(0, len(wasm), 12):
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in wasm[i:i + 12]) + ",")
    lines.append("};")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"dt_wasm.h 生成：{out}（{len(wasm)}B）", flush=True)


async def main() -> None:
    cfg = load_config("/home/emb/project/Tessera/agent/config.toml")
    ctx = AppContext(cfg=cfg, budget=ContextBudget(window_tokens=cfg.context_window))
    spec = SPEC_PATH.read_text(encoding="utf-8")

    print("=== [1/2] app_develop（真 LLM 生成 → 线程档编译 → TSAP 打包） ===", flush=True)
    t0 = time.time()
    dev = await app_develop(ctx, spec, KEY, out_dir=OUT_DIR,
                            log_fn=lambda s: print(f"  {s}", flush=True))
    pkg = Path(dev["package_path"])
    print(f"develop 完成 {time.time()-t0:.1f}s：pkg={pkg} size={dev['package_size']}B "
          f"compiled={dev['compiled']}", flush=True)

    print("=== [2/2] wasm 提取 + dt_wasm.h 生成 + demo 资产归档 ===", flush=True)
    wasm = tsap_extract_wasm(pkg)
    gen_wasm_header(wasm, PKG_HDR)
    DEMO_DIR.mkdir(parents=True, exist_ok=True)
    (DEMO_DIR / pkg.name).write_bytes(pkg.read_bytes())
    (DEMO_DIR / "app.wasm").write_bytes(wasm)
    src = pkg.with_name("app.c")
    if src.exists():
        (DEMO_DIR / "app.c").write_text(src.read_text())
    print("done", flush=True)


asyncio.run(main())
