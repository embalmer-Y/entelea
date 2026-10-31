# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""demo-thermo（原 D-AG7）全链演示驱动（通用域解放 Agent 批 AG7 验收）：
spec → **创作循环路径**（真 LLM：计划 → ws_* 多文件编辑 → 编译/面检查证据回喂
→ 收敛 → 签名打包）→ zenoh 无线部署 → 串口判据行抓取。

用法（agent-venv python，需先 source ~/project/logs/env.sh 提供 ANTHROPIC_*）。
验证对象 = AG2-AG6 全栈（Workspace/ws_* 循环/验证器/快照/app_develop 双路径）。
"""
import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, "/home/emb/project/Tessera/agent")

from tessera_agent.common.config import load_config
from tessera_agent.common.limits import ContextBudget
from tessera_agent.platform import AppContext
from tessera_agent.domain.app_chain import app_develop, app_deploy

KEY_PUB = (
    "/home/emb/project/Tessera/agent/tests/fixtures/ts-test-root.key",
    "/home/emb/project/Tessera/agent/tests/fixtures/ts-test-root.pub",
)
SPEC_PATH = Path("/home/emb/project/Tessera/agent/demos/demo-thermo/spec.md")
EVIDENCE = Path("/home/emb/project/Tessera/docs/evidence/ag7_console.txt")
CRITERIA = "AG7 DONE cycles=200"


async def main() -> int:
    cfg = load_config("/home/emb/project/Tessera/agent/config.toml")
    ctx = AppContext(cfg=cfg, budget=ContextBudget(window_tokens=cfg.context_window))
    spec = SPEC_PATH.read_text(encoding="utf-8")

    print("=== [1/3] app_develop 创作循环（真 LLM 多文件工程） ===", flush=True)
    t0 = time.time()
    dev = await app_develop(ctx, spec, KEY_PUB[0],
                            log_fn=lambda s: print(f"  {s}", flush=True))
    elapsed = time.time() - t0
    print(f"循环完成 {elapsed:.1f}s：state={dev['state']} rounds={dev['rounds']} "
          f"task_id={dev['task_id']}", flush=True)
    print(f"  workspace={dev['workspace']}", flush=True)
    print(f"  compile_facts={ {k: v for k, v in dev['compile_facts'].items() if k != 'files_sha256'} }",
          flush=True)
    # 数值采集（Q-39 CG4 输入——large 档数值数据点）
    ws_proj = Path(dev["workspace"]) / "project.json"
    if ws_proj.is_file():
        import json
        files = json.loads(ws_proj.read_text()).get("files", [])
        print(f"  工程文件：{files}", flush=True)

    print("=== [2/3] 无线部署（zenoh → deploybench） ===", flush=True)
    dep = await app_deploy(ctx, dev["package_path"], KEY_PUB[1],
                           log_fn=lambda s: print(f"  {s}", flush=True))
    print(f"部署确认：{dep['node']}/{dep['cube']}", flush=True)

    print("=== [3/3] 串口判据抓取（90s 窗） ===", flush=True)
    import serial
    hit_lines: list[str] = []
    console: list[str] = []
    t0 = time.time()
    with serial.Serial("/dev/ttyACM0", 115200, timeout=1) as s:
        while time.time() - t0 < 90:
            line = s.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            console.append(line)
            if "ag7" in line.lower() or "AG7" in line:
                print(f"  >>> {line}", flush=True)
            if CRITERIA in line:
                hit_lines.append(line)
                break
    EVIDENCE.parent.mkdir(parents=True, exist_ok=True)
    EVIDENCE.write_text("\n".join(console) + "\n", encoding="utf-8")
    print(f"console 留档：{EVIDENCE}（{len(console)} 行）", flush=True)
    if hit_lines:
        print(f"AG7 PASS：{hit_lines[0]}", flush=True)
        return 0
    print("AG7 FAIL：判据行未命中", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
