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

- 候选 = digest（整文件 sha256，同 Forge `package_digest`）+permissions
  +version+**slot**+**epoch**。NONE → PENDING_CONFIRM（submit 分配 epoch：
  非 0、单调、跨 end/restore 不回退，回绕跳过 0）；PENDING → ACTIVE
  （`confirm(&shown)`——**唯一晋升路径，用户显式**；shown 须与当前候选
  全字段一致，否则 E_PERM 且保持 PENDING：旧展示/重推后的过期确认不
  生效）/ REJECTED（reject）/ TIMEOUT（fail-closed 撤销）；ACTIVE → NONE
  （end）。
- 单候选与槽保留：ACTIVE/PENDING 期 submit=E_BUSY；REJECTED/TIMEOUT
  只可同槽重推（他槽=E_BUSY）；slot 域外=E_PARAM；非预期迁移=E_STATE。
- **执行门（RF1）**：`entelea_trial_gate(slot, digest)` 由 wasm 宿主在
  装载（卸旧之前）与每次调用时查询：候选槽（或恢复后的 UNKNOWN 槽）
  仅 ACTIVE 且槽内当前字节 digest 相符=OK，其余（PENDING/REJECTED/
  TIMEOUT/确认后换字节）=E_PERM。NONE 或非候选槽=OK（不受试用治理）。
- **工程通过≠用户验收**：intake/验签通过只到 PENDING——confirm 之前
  该槽不可执行。产品构建无自动确认；`FACTORY_DEV_AUTOCONFIRM` 仅为
  开发夹具，无用户验收效力。

## 工厂 intake（fail-closed）

- 未知 payload_kind 拒（不默认解释为 wasm/不按后缀推断权限——
  R05 同纪律）；预算域外拒（0<budget≤1e7、0<mem≤512KB——与
  Lua 工厂合同同域）。

## 跨重启持久（板级批 A-4 已交付）

- `flush/restore` 经 R03 meta 单槽（本层独占——pkg active 已迁 KV，
  review P1-1）。v2 记录 42B 小端：ver=2|state|cand_version|slot|perm16|
  last_epoch32|digest[32]。
- 恢复 fail-closed（RF1）：长度/版本（含旧 v1）/state 越界/非 NONE 时
  slot≥2 或 epoch=0 → **TIMEOUT + slot UNKNOWN**（门拒一切受治理槽，
  不复活垃圾也不放开为 NONE）；TIMEOUT/REJECTED 约束态跨重启保持。
  双副本皆不可读 → NONE（与“从未写入”不可区分——残余）。
  真机证据：r12-validation A-4 节（v1 时期）；v2 由 RF1 native_sim
  双启动复验。

## 未测与边界

SDK 实际接入/版本冻结/隔离与外发（获批工厂任务）；试用数据晋升
（D05 细节）；多候选（D05=单候选已批）；设备/Forge 字节对拍真机批。
RF1 残余：trial_end 后槽内字节不受试用治理（稳定集准入=D05 待定）；
digest 计算与模块装载之间存在单线程 TOCTOU 窗口（同线程无并发写槽，
多线程写槽须另行加锁）；`sha256_stream` 全局上下文仅单线程。
坏记录恢复为 UNKNOWN 后允许向任一槽重推（否则试用通道永久锁死），
重推后另一槽回到不受治理——原候选槽信息已随坏记录丢失。
