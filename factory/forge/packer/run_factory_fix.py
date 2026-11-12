#!/usr/bin/env python3
"""D-3 反馈修正轮：设备闸（WAMR fast interp）拒绝 run001 的模块——
WASM 验证失败（spin 导出的 br_if 分支栈型不匹配：无 loop 包裹、
br_if 0 直指函数尾 label 而栈上无 result 值）。附错误事实让 agent 修。"""

raise SystemExit(
    "retired (RF1): one-shot run002/run003 driver; it bypasses the budget gate and would overwrite evidence. Use run_factory.py."
)
import json
import sys
import time
from pathlib import Path

from openhands.sdk import LLM, Agent, Conversation, Tool
from openhands.tools.file_editor import FileEditorTool
from openhands.tools.terminal import TerminalTool

CRED = Path("/home/emb/project/.cloud-credentials.json")
LEDGER = Path("/home/emb/project/.cloud-usage/ledger.md")
FORGE = Path(__file__).resolve().parent.parent
RUN_DIR = FORGE / "artifacts" / "run002"
MODEL = "bigmodel/glm-5.3-flash"

BRIEF = (FORGE / "tasks/factory-brief.md").read_text()
FEEDBACK = """## 上一轮产物被设备验证器拒绝（run001）

app.wasm 在设备 WAMR（fast interpreter）wasm_runtime_load 阶段被拒
（模块验证失败）。分析（当前字节）：

- `spin` 导出体无 `loop` 指令，`br_if 0` 直接指向函数尾 label，
  而该 label 需要 result i32——分支路径栈上无 i32 值，**验证失败**。

## 修正要求

重写 `app.wasm`（覆盖同一交付路径）。`spin` 必须形如：

    (func (export "spin") (param i32) (result i32)
      (loop $l
        (br_if $l (i32.gt_s (local.get 0) (i32.const 0)))
        (local.set 0 (i32.sub (local.get 0) (i32.const 1))))
      (i32.const 0))

或任何**通过 wasm 验证**的等价实现。交付前自检建议：逐段核对
type/function/export/code 四段一致性；每函数体以 locals 计数开头、
以 end 结尾；分支深度合法。
"""


def main() -> int:
    cred = json.loads(CRED.read_text())
    llm = LLM(
        model=f"openai/{MODEL}",
        api_key=cred["llm_api_key"],
        base_url=cred["llm_base_url"],
    )
    agent = Agent(
        llm=llm, tools=[Tool(name=FileEditorTool.name), Tool(name=TerminalTool.name)]
    )
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    # run001 产物拷入 run002 作为修改起点
    (RUN_DIR / "app.wasm").write_bytes(
        (FORGE / "artifacts/run001/app.wasm").read_bytes()
    )
    conv = Conversation(agent=agent, workspace=str(FORGE))
    conv.send_message(
        BRIEF + f"\n\n## 本次 run 交付目录\n\n{RUN_DIR}/app.wasm"
        " —— 覆盖修改这一个文件；完成后停止。\n\n" + FEEDBACK
    )
    t0 = time.time()
    conv.run()
    dt = time.time() - t0
    data = (RUN_DIR / "app.wasm").read_bytes()
    if len(data) > 64 * 1024 or data[:8] != b"\x00asm\x01\x00\x00\x00":
        print(f"FAIL: {len(data)}B 头={data[:4].hex()}", file=sys.stderr)
        return 1
    tokens = 0
    for m in conv.conversation_stats.usage_to_metrics.values():
        u = m.accumulated_token_usage
        if u:
            tokens += u.prompt_tokens + u.completion_tokens
    cost = tokens * 4.0 / 1_000_000
    with LEDGER.open("a") as f:
        f.write(
            f"| {time.strftime('%Y-%m-%d')} | 工厂 run002（SDK 修 spin） "
            f"| {MODEL} | {tokens} | ¥{cost:.3f} "
            f"| app.wasm {len(data)}B / {dt:.0f}s |\n"
        )
    print(f"OK: run002 app.wasm {len(data)}B，{dt:.0f}s，tokens≈{tokens}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
