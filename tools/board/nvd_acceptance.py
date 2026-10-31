# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""NV 全链演示驱动（新机器首跑验收金标准，checklist 分层验证⑦）：
spec → 真 LLM app_develop → TSAP 打包 → app_deploy（zenoh 无线部署）→
暖复位 → console 判据验收（NVD-6 同款流程）。

板型感知参数化（R4 · DEC-64③）：判据窗 = 复位后到主判据行的实测预算，
随板 boot 时长差异参数化——xiao/deploybench 自检轻（默认 18s）；atkbench
（音频/LCD/LVGL 自检）实测 boot ~45-50s → 用 --window 90。板目标/串口/
路径随参，无主机硬编码（路径自仓库根派生）。

用法（agent-venv python，需先 source env.sh 提供 ANTHROPIC_*）：
  python3 tools/board/nvd_acceptance.py                       # xiao 缺省口径
  python3 tools/board/nvd_acceptance.py --window 90           # atk 口径
  python3 tools/board/nvd_acceptance.py --node akn --cube akc --port /dev/ttyACM0
"""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "agent"))

from tessera_agent.common.config import load_config
from tessera_agent.common.limits import ContextBudget
from tessera_agent.platform import AppContext
from tessera_agent.domain.app_chain import app_develop, app_deploy

SPEC = """【需求】立方体节拍巡检 APP（app_id 形如 com.example.cube.nvd）：周期巡检计数器——
启动时打印初始化行；此后每个周期（500ms）tick 计数 +1 并打印一行计数值；
计数达到 10 后打印完成行（含最终计数值），之后保持运行但不再打印周期行。

【资源】无硬件输出通道（不申请 gpio/pwm/adc/fs/av 权限）；仅使用 ts_log 输出；
周期来源 = 框架 tick 回调（app_tick）；确定性要求：同输入序列同输出。

【判据】主判据（完成行精确匹配）："NVD DONE ticks=10"；
附属证据行（前缀匹配）："nvd init"、"nvd tick cnt="。
计数值输出必须两位补零（含 0 值必须输出字符 '0'）；单次 log 输出完整行。

【安全声明】本 APP 无物理输出通道，不触及安全合同（三安全态/限幅/estop 无交互）；
不使用随机数与墙钟（确定性）；无 WASI/stdio/全局构造。"""


def capture_and_verdict(port: str, window_s: float) -> tuple[bool, list[str], list[str]]:
    import serial

    ser = serial.Serial(port, 115200, timeout=1.0)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.05)
    ser.setDTR(True)
    ser.reset_input_buffer()
    deadline = time.time() + window_s
    lines: list[str] = []
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        lines.append(raw.decode("utf-8", errors="replace").rstrip())
    ser.close()
    hits = [ln for ln in lines if any(k in ln for k in ("NVD", "nvd", "DB", "ts-net", "appmgr"))]
    main_ok = any("NVD DONE ticks=10" in ln for ln in lines)
    sub_init = any("nvd init" in ln for ln in lines)
    sub_tick = any("nvd tick cnt=" in ln for ln in lines)
    ok = main_ok and sub_init and sub_tick
    return ok, lines, hits


async def main() -> None:
    ap = argparse.ArgumentParser(description="NVD 全链演示驱动（板型感知）")
    ap.add_argument("--window", type=float, default=18.0,
                    help="判据窗秒数（复位后抓取时长）：xiao=18 缺省；atkbench=90（boot ~45-50s）")
    ap.add_argument("--node", default="dbn")
    ap.add_argument("--cube", default="dbc")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--out", default=str(REPO.parent / "logs" / "nvd_acceptance_console.txt"),
                    help="console 判据行落盘路径（仓外）")
    args = ap.parse_args()

    cfg = load_config(str(REPO / "agent" / "config.toml"))
    ctx = AppContext(cfg=cfg, budget=ContextBudget(window_tokens=cfg.context_window))
    key = str(REPO / "agent" / "tests" / "fixtures" / "ts-test-root.key")
    out_dir = str(REPO / "agent" / "build")

    print("=== [1/3] app_develop（真 LLM 生成 → 编译 → TSAP 打包） ===", flush=True)
    t0 = time.time()
    dev = await app_develop(ctx, SPEC, key, out_dir=out_dir, log_fn=lambda s: print(f"  {s}", flush=True))
    print(
        f"develop 完成 {time.time()-t0:.1f}s：pkg={dev['package_path']} "
        f"size={dev['package_size']}B compiled={dev['compiled']}",
        flush=True,
    )

    print("=== [2/3] app_deploy（zenoh 发现 → 租约 → 分块 → 激活 → 复位核验） ===", flush=True)
    dep = await app_deploy(
        ctx, dev["package_path"], dev["pub_key_path"], node=args.node, cube=args.cube,
        log_fn=lambda s: print(f"  {s}", flush=True),
    )
    print("deploy 完成：" + json.dumps(dep, ensure_ascii=False, default=str)[:400], flush=True)

    print(f"=== [3/3] 真机 console 判据验收（RTS 复位 + 抓取，窗 {args.window}s） ===", flush=True)
    ok, lines, hits = capture_and_verdict(args.port, args.window)
    for ln in hits:
        print(f"  | {ln}", flush=True)
    print(f"\n=== 判据：{'NVD PASS' if ok else 'NVD FAIL'}（主判据 + 2 附属前缀） ===", flush=True)
    with open(args.out, "w") as f:
        f.write("\n".join(lines))
    sys.exit(0 if ok else 1)


asyncio.run(main())
