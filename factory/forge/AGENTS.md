# Forge 工作区 — Agent 记忆

## 环境
- 无 wat2wasm/wasm-tools/wasmtime；仅 Python（系统 + openhands 环境 3.12）。
  wasm 模块须用 Python 手写字节产出（工厂任务书明确允许）。
- 工具链 venv：`/home/emb/project/envs/openhands`。

## app.wasm 手写字节布局（run004 现行，96B；packer/wasm_gen.py 为准）
段序：type(1) → func(3) → export(7) → code(10)；无 import/memory/table/
global/start/elem/data 段（设备 WASI 关、池模式无 memory 段）。
单一类型 `(i32)->(i32)`（0x60 01 7f 01 7f），三函数 run/spin/hang 全用类型 0。

- run: `local.get 0; i32.const 2; i32.mul; i32.const 1; i32.add`
  （即 2N+1，与 packer/wasm_gen.py 一致）
- spin（RF1 #10 修正形，packer/wasm_gen.py 为准）：
  `block; loop; local.get0; i32.const0; i32.le_s; br_if 1; local.get0;
   i32.const1; i32.sub; local.set0; br 0; end; end; i32.const0`
  旧形（gt_s+br_if 0 在递减之前回边）对 N>=1 死循环——只测 spin(0) 不暴露，
  须以 N>=1 对拍。分支深度只指向 void 标签（不得指向函数尾，run001 败因）。
- hang: `loop(void); br 0; end`（br 对 void 标签无需栈值 → 合法死循环）。

## 自检脚本（/tmp，会话丢失后可重建）
- `check_app_wasm.py <file>`：结构自检——段序/四段一致性/每函数 locals
  计数开头+end 结尾/块平衡/分支深度/禁用特性扫描（浮点、memory.grow、
  ref/thread 操作码、禁用段）。
- `validate_app_wasm.py <file>`：按 wasm 核心规范实现的栈式验证器（覆盖
  本模块操作码子集，含 unreachable 多态规则），对三个函数做逐指令栈
  纪律验证；附负控制（run001 破损 spin 形 → 正确拒绝）。
- run001 败因：spin 导出体无 loop，br_if 0 指向函数尾 label（需 i32），
  栈上无值 → WAMR wasm_runtime_load 模块验证失败。

## 纪律
- 只改 `artifacts/<run>/` 下产物；不碰 packer/、tasks/。
- 密钥/凭据不出工作区（见 README.md 边界条款）。