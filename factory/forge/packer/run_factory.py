#!/usr/bin/env python3
"""D-3 工厂 SDK 驱动：OpenHands agent 产 wasm App（打包签名由 tsap_pack.py）。

凭据：/home/emb/project/.cloud-credentials.json（0600，仓库外；不打印
不入库）。额度：工厂线 ≤18M token（README 软分配），逐笔记
/home/emb/project/.cloud-usage/ledger.md（¥4/M token 计价）。

RF1 #10 硬闸：
- 运行前：台账"工厂"行累计——任一行不可解析/台账缺失 → 拒绝运行；
  已用 ≥ 总额、或 已用+本轮上限 > 总额 → 拒绝；预计 ≥80% → 预警。
- 运行中：SDK max_budget_per_run（自定价 1/token → 成本单位=token）
  + max_iteration_per_run；逐步检查，超出至多一步。
- SDK 仅在闸通过后导入；用量无论成败都落台账，读不出按本轮上限记。
残余（owner）：SDK 宿主隔离（终端工具=宿主 shell）、Forge 版本库。

用法：run_factory.py <run_dir> [--run-tokens N] [--max-iter N]
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

CRED = Path("/home/emb/project/.cloud-credentials.json")
LEDGER = Path("/home/emb/project/.cloud-usage/ledger.md")
FORGE = Path(__file__).resolve().parent.parent
MODEL_DEFAULT = "bigmodel/glm-5.3-flash"  # allowed_models[0] 兜底
FACTORY_CAP_TOKENS = 18_000_000
WARN_RATIO = 0.8
RUN_TOKENS_DEFAULT = 2_000_000
MAX_ITER_DEFAULT = 60
YUAN_PER_TOKEN = 4.0 / 1_000_000

_TERM = re.compile(r"^(\d+(?:\.\d+)?)([KkMm]?)$")


class BudgetError(Exception):
    pass


def parse_tokens(cell: str) -> int:
    """台账 tokens 格：1025161 / ~1.0M(估) / 63+76 / 1.5K。"""
    s = cell
    for mark in ("(估)", "（估）", "~", "≈", ",", " "):
        s = s.replace(mark, "")
    if not s:
        raise BudgetError(f"unparseable token cell: {cell!r}")
    total = 0
    for term in s.split("+"):
        m = _TERM.match(term)
        if not m:
            raise BudgetError(f"unparseable token cell: {cell!r}")
        scale = {"": 1, "K": 1_000, "M": 1_000_000}[m.group(2).upper()]
        total += int(round(float(m.group(1)) * scale))
    return total


def factory_tokens_used(text: str) -> int:
    used = 0
    for line in text.splitlines():
        if not line.startswith("|") or "工厂" not in line:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 4 or "工厂" not in cells[1]:
            raise BudgetError(f"malformed factory ledger row: {line!r}")
        used += parse_tokens(cells[3])
    return used


def budget_gate(used: int, run_tokens: int,
                cap: int = FACTORY_CAP_TOKENS) -> str | None:
    """超限抛 BudgetError；返回预警文本或 None。"""
    if run_tokens <= 0:
        raise BudgetError("per-run token cap must be > 0")
    if used >= cap:
        raise BudgetError(f"factory budget exhausted: {used}/{cap}")
    if used + run_tokens > cap:
        raise BudgetError(
            f"projected {used}+{run_tokens} exceeds factory cap {cap}")
    if used + run_tokens >= WARN_RATIO * cap:
        return f"factory budget >=80% after this run: {used + run_tokens}/{cap}"
    return None


def ledger_row(run: str, model: str, tokens: int | None, run_cap: int,
               note: str) -> str:
    if tokens is None:
        cell, cost = f"~{run_cap}(估)", f"≈¥{run_cap * YUAN_PER_TOKEN:.3f}(估)"
        note += "；usage 未读出，按本轮上限记"
    else:
        cell, cost = str(tokens), f"¥{tokens * YUAN_PER_TOKEN:.3f}"
    return (f"| {time.strftime('%Y-%m-%d')} | 工厂 {run}（SDK 产 wasm） "
            f"| {model} | {cell} | {cost} | {note} |\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("--run-tokens", type=int, default=RUN_TOKENS_DEFAULT)
    ap.add_argument("--max-iter", type=int, default=MAX_ITER_DEFAULT)
    args = ap.parse_args(argv)

    try:
        if not LEDGER.is_file():
            raise BudgetError(f"ledger missing: {LEDGER}")
        used = factory_tokens_used(LEDGER.read_text())
        warn = budget_gate(used, args.run_tokens)
    except BudgetError as exc:
        print(f"FAIL: budget gate: {exc}", file=sys.stderr)
        return 2
    if warn:
        print(f"warn: {warn}", file=sys.stderr)
    if args.max_iter <= 0:
        print("FAIL: --max-iter must be > 0", file=sys.stderr)
        return 2
    run_dir = args.run_dir.resolve()
    if run_dir.parent != (FORGE / "artifacts").resolve():
        print(f"FAIL: run_dir must be {FORGE}/artifacts/<run>", file=sys.stderr)
        return 2
    wasm = run_dir / "app.wasm"
    if wasm.exists():
        print(f"FAIL: {wasm} exists (prior evidence is not overwritten)",
              file=sys.stderr)
        return 2

    from openhands.sdk import LLM, Agent, Conversation, Tool
    from openhands.tools.file_editor import FileEditorTool
    from openhands.tools.terminal import TerminalTool

    cred = json.loads(CRED.read_text())
    model = cred.get("model", MODEL_DEFAULT)
    llm = LLM(model=f"openai/{model}", api_key=cred["llm_api_key"],
              base_url=cred["llm_base_url"],
              input_cost_per_token=1.0, output_cost_per_token=1.0)
    agent = Agent(llm=llm, tools=[Tool(name=FileEditorTool.name),
                                  Tool(name=TerminalTool.name)])
    run_dir.mkdir(parents=True, exist_ok=True)
    conv = Conversation(agent=agent, workspace=str(FORGE),
                        max_iteration_per_run=args.max_iter,
                        max_budget_per_run=float(args.run_tokens))
    brief = (FORGE / "tasks/factory-brief.md").read_text()
    task = (brief + "\n\n## 本次 run 的交付目录（绝对路径）\n\n"
            f"{wasm} —— 只写这一个文件；完成后停止。\n")
    t0 = time.time()
    note = "未交付"
    rc = 1
    try:
        conv.send_message(task)
        conv.run()
        if not wasm.exists():
            print("FAIL: agent 未交付 app.wasm", file=sys.stderr)
        else:
            data = wasm.read_bytes()
            if (len(data) > 64 * 1024
                    or data[:8] != b"\x00asm\x01\x00\x00\x00"):
                note = f"app.wasm 形状不符 {len(data)}B"
                print(f"FAIL: {note}", file=sys.stderr)
            else:
                note, rc = f"app.wasm {len(data)}B", 0
    finally:
        tokens = None
        try:
            n = 0
            for m in conv.conversation_stats.usage_to_metrics.values():
                u = m.accumulated_token_usage
                if u:
                    n += u.prompt_tokens + u.completion_tokens
            tokens = n or None  # 0 = 未读出（宁多记不少记）
        except Exception as exc:  # noqa: BLE001 —— 读不出按上限记
            print(f"warn: usage 读取失败：{exc}", file=sys.stderr)
        with LEDGER.open("a") as f:
            f.write(ledger_row(run_dir.name, model, tokens, args.run_tokens,
                               f"{note} / {time.time() - t0:.0f}s"))
    print(f"{'OK' if rc == 0 else 'FAIL'}: {note}，tokens≈{tokens}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
