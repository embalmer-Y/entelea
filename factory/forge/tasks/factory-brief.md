# 工厂任务书：最小 Wasm App（Forge run 规格）

## 目标

产出一个可被 ENTELEA 设备（WAMR fast 解释器、WASI 关、指令配额）
装载执行的 WebAssembly 模块 `app.wasm`，作为工厂签名的最小 App。

## 模块规格（精确）

1. 合法 wasm 二进制（magic `\0asm` ver1）；总字节 ≤64KB。
2. **不使用任何导入**（WASI 关闭；宿主 natives 仅后续批次扩展）。
3. 导出一个函数 `(func (export "run") (param i32) (result i32))`：
   语义=返回 `2*N+1`（N=入参）。用本地算法实现，不得调用导入。
4. 额外导出 `(func (export "spin") (param i32) (result i32))`：空转
   循环 N 次后返回 0。
5. 额外导出 `(func (export "hang") (param i32) (result i32))`：**无条件
   死循环**（`loop … br 0`，无视入参）——设备步预算到期→E_TIMEOUT
   有界停止夹具（R-V1：真死循环对拍，替代"预算 0"技巧证据）。
6. 禁用：浮点（设备编译面无 SIMD/浮点承诺）、memory.grow（池模式
   256KB）、线程/引用类型。

## 交付物

- `artifacts/<run>/app.wasm` —— 模块二进制
- 不得修改 packer/、tasks/、以及两仓库任何内容。

## 验证（SDK 自检后交付）

工具链无 wat2wasm 时允许直接手写/生成 wasm 二进制（最小模块手写字节
可行）。交付前自检：二进制以 `00 61 73 6d 01 00 00 00` 开头。
