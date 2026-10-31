# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""demo-pdm-audio 全链演示驱动（原 D-PDM）（V2 纵深批 M18，DEC-54② 既定③执行）：
spec → 真 LLM app_develop（编译+TSAP 打包）→ dp_pkg.h 机械生成（pdmdemo 嵌入）。
烧录/判据由外部编排（west build/flash + pdm_recv.py + console 抓取）。

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
SPEC_PATH = Path("/home/emb/project/Tessera/agent/demos/demo-pdm-audio/spec.md")
PKG_HDR = Path("/home/emb/project/Tessera/firmware/tests/pdmdemo/src/dp_pkg.h")


def gen_pkg_header(tsap: Path, out: Path) -> None:
    """TSAP → C 数组头（机械生成——gen_dav_pkg.sh 同纪律，教训 27 反手抄）。"""
    blob = tsap.read_bytes()
    lines = [f"/* 机械生成（demo_pdm.py）——{tsap.name}，{len(blob)}B */",
             f"static const uint8_t dp_pkg[{len(blob)}] = {{"]
    for i in range(0, len(blob), 12):
        chunk = blob[i:i + 12]
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"dp_pkg.h 生成：{out}（{len(blob)}B）", flush=True)


async def main() -> None:
    cfg = load_config("/home/emb/project/Tessera/agent/config.toml")
    ctx = AppContext(cfg=cfg, budget=ContextBudget(window_tokens=cfg.context_window))
    spec = SPEC_PATH.read_text(encoding="utf-8")

    print("=== [1/2] app_develop（真 LLM 生成 → 编译 → TSAP 打包） ===", flush=True)
    t0 = time.time()
    dev = await app_develop(ctx, spec, KEY, out_dir=OUT_DIR,
                            log_fn=lambda s: print(f"  {s}", flush=True))
    pkg = Path(dev["package_path"])
    print(f"develop 完成 {time.time()-t0:.1f}s：pkg={pkg} size={dev['package_size']}B "
          f"compiled={dev['compiled']}", flush=True)

    print("=== [2/2] dp_pkg.h 机械生成（pdmdemo 嵌入用） ===", flush=True)
    gen_pkg_header(pkg, PKG_HDR)
    demo_dir = Path("/home/emb/project/Tessera/agent/demos/demo-pdm-audio")
    demo_dir.mkdir(parents=True, exist_ok=True)
    for name in (pkg.name, pkg.with_suffix(".wasm").name):
        src = pkg.with_name(name) if (pkg.parent / name).exists() else None
        if src and src.exists():
            (demo_dir / src.name).write_bytes(src.read_bytes())
            print(f"demo 资产入仓目录：{src} → {demo_dir / src.name}", flush=True)
    print("done", flush=True)


asyncio.run(main())
