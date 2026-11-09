# R12 工厂/试用/晋升合同

新-only；方向与未决项以[执行规划](../../../docs/rebuild-plan.md)第 7
节与 7.1（工厂引擎已确认决策）为准。

## 编程引擎（owner 已确认）

- **OpenHands Software Agent SDK** 承担通用编程循环（代码探索/编辑/
  规划/上下文/工具调用）；**不在固件内**、不进 MCU 公共库依赖。
- Forge 掌握：能力/ABI、Wasm/Lua 产物约束、预算、独立构建/仿真
  证据、签名隔离与交付。设备保留权限/保护/试用/用户验收。
- **本批固件面**：工厂产物 intake 契约 + 试用状态机。SDK 实际接入
  （版本/隔离/外发/费用/取消恢复）在获批工厂任务中执行——本批
  不安装 SDK、不建新 Forge 工作区（7.1 推进顺序）。

## 试用状态机（D05 已批方向：稳定/候选分离、单候选）

- NONE → PENDING_CONFIRM（submit：**用户确认对象=digest+permissions+
  version**——内容绑定）；PENDING → ACTIVE（confirm——**唯一晋升
  路径，用户显式**）/ REJECTED（reject——槽保留可重推）/ TIMEOUT
  （fail-closed 撤销）；ACTIVE → NONE（end——晋升或放弃后清槽）。
- ACTIVE 期 submit=E_BUSY（单候选）；任何非预期迁移=E_STATE。
- **工程通过≠用户验收**：intake/验签通过只到 PENDING——
  confirm 之前不生效。

## 工厂 intake（fail-closed）

- 未知 payload_kind 拒（不默认解释为 wasm/不按后缀推断权限——
  R05 同纪律）；预算域外拒（0<budget≤1e7、0<mem≤512KB——与
  Lua 工厂合同同域）。

## 跨重启持久（板级批 A-4 已交付）

- `flush/restore` 经 R03 meta 单槽（本层独占——pkg active 已迁 KV，
  review P1-1）；恢复 fail-closed（坏记录→NONE 不复活垃圾）；
  TIMEOUT/REJECTED 约束态跨重启保持（重置不可绕过 fail-closed）。
  真机证据：r12-validation A-4 节。

## 未测与边界

SDK 实际接入/版本冻结/隔离与外发（获批工厂任务）；试用数据晋升
（D05 细节）；多候选（D05=单候选已批）；设备/Forge 字节对拍真机批。
