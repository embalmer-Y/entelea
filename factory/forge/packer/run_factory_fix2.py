#!/usr/bin/env python3
"""D-3 反馈修正第三轮：run002 仍被 WAMR 验证拒（hang 尾值缺失）。
一次说清三缺陷+逐函数 wat 模板（字节级照抄生成）。"""

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
RUN_DIR = FORGE / "artifacts" / "run003"
MODEL = "bigmodel/glm-5.3-flash"

TASK = """重写 app.wasm（WebAssembly 二进制，手写字节）。必须**逐函数**按
以下 wat 模板生成（等价实现亦可，但必须通过 wasm 验证）：

    (module
      (func (export "run") (param i32) (result i32)
        (i32.add (i32.mul (local.get 0) (i32.const 2)) (i32.const 1)))
      (func (export "spin") (param i32) (result i32)
        (loop $l
          (br_if $l (i32.gt_s (local.get 0) (i32.const 0)))
          (local.set 0 (i32.sub (local.get 0) (i32.const 1))))
        (i32.const 0))
      (func (export "hang") (param i32) (result i32)
        (loop $l (br $l))
        (i32.const 0)))

## 前两轮被设备验证器拒绝的具体缺陷（勿再犯）

1. run001 spin：无 loop 包裹、br_if 0 直指函数尾而栈上无 i32——验证拒。
2. run002 hang：loop 内 br 0 无限回边后函数尾无 i32.const 0——
   **wasm 验证器要求静态栈型满足 result**（不可达也必须补：loop 后
   加 (i32.const 0)）。
3. run002 run 语义=3N+1（N*2+N+1）——要求 2N+1。
4. run002 spin 回边不减计数（br_if 后未执行减法即跳回）——回边
   条件与计数递减须同循环体协调（模板顺序：判→br_if→减→loop 尾）。

约束：无导入；总长 ≤64KB；交付前自检四段（type/function/export/code）
一致性与每函数体（locals 计数开头、end 结尾、result 静态满足）。

交付：RUN_DIR/app.wasm（覆盖）——只写这一个文件，完成即止。
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
    (RUN_DIR / "app.wasm").write_bytes(
        (FORGE / "artifacts/run002/app.wasm").read_bytes()
    )
    conv = Conversation(agent=agent, workspace=str(FORGE))
    conv.send_message(TASK.replace("RUN_DIR", str(RUN_DIR)))
    t0 = time.time()
    conv.run()
    dt = time.time() - t0
    data = (RUN_DIR / "app.wasm").read_bytes()
    if len(data) > 64 * 1024 or data[:8] != b"\x00asm\x01\x00\x00\x00":
        print(f"FAIL: {len(data)}B", file=sys.stderr)
        return 1
    tokens = 0
    for m in conv.conversation_stats.usage_to_metrics.values():
        u = m.accumulated_token_usage
        if u:
            tokens += u.prompt_tokens + u.completion_tokens
    cost = tokens * 4.0 / 1_000_000
    with LEDGER.open("a") as f:
        f.write(
            f"| {time.strftime('%Y-%m-%d')} | 工厂 run003（SDK 三缺陷反馈） "
            f"| {MODEL} | {tokens} | ¥{cost:.3f} "
            f"| app.wasm {len(data)}B / {dt:.0f}s |\n"
        )
    print(f"OK: run003 app.wasm {len(data)}B，{dt:.0f}s，tokens≈{tokens}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
