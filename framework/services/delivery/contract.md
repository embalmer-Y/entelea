# R12 工厂/试用/晋升合同

新-only；方向与未决项以[执行规划](../../../docs/rebuild-plan.md)第 7
节与 7.1（工厂引擎已确认决策）为准。

## 编程引擎（owner 已确认）

- **OpenHands Software Agent SDK** 承担通用编程循环（代码探索/编辑/
  规划/上下文/工具调用）；**不在固件内**、不进 MCU 公共库依赖。
- Forge 掌握：能力/ABI、Wasm/Lua 产物约束、预算、独立构建/仿真
  证据、签名隔离与交付。设备保留权限/保护/试用/用户验收。
- 固件面是工厂产物 intake 契约 + 试用状态机。后续获批工厂任务已在
  独立工作区接入 SDK 并开展试验；完整隔离、外发、费用、取消/恢复和
  独立版本锚点不能仅由接入成功证明。

## 试用状态机（D05 已批方向：稳定/候选分离、单候选）

- 候选 = digest（整文件 sha256，同 Forge `package_digest`）+permissions
  +version+**slot**+**epoch**。NONE → PENDING_CONFIRM（submit 分配 epoch：
  非 0、单调、跨 end/restore 不回退；到 `UINT32_MAX` 后新 submit
  返回 `E_RANGE` 且不更改状态，不能回绕复用旧确认）；PENDING → ACTIVE
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
- **权限绑定缺口（RF1，未闭合）**：确认时比较保存的 `permissions`
  位图，但未与签名 manifest 或实际装载的 principal 权限表绑定。
  真实 WAMR 测试可构造确认权限为 0、宿主 principal 仍具 GPIO 读权限
  并通过 native 裁决的反例；这不是物理 GPIO 操作证据。预算字段仅检查
  数值范围，也未证明与签名内容和实际执行预算一致。此门不能作为
  “用户确认了运行时全部权限和预算”的证据；位图映射、可信绑定及
  签名/预算合同仍需明确后修复。
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
  slot≥2 或 epoch=0 → **E_IO、TIMEOUT + slot UNKNOWN**（两槽门拒）。
  flush 将同一状态写入两副本并回读对拍；恢复只接受两份均有效且内容
  一致的记录。单份有效、两份不一致或读取失败均报 E_IO、保持 UNKNOWN，
  防止回退到更宽松的旧状态；两份都无效时，只有完整逻辑区可读且全为
  擦除值才归 NONE。全擦除本身无法证明该分区从未被人为或异常擦除。
  旧版本可能只写过单份或留下两份不同记录，升级后将受限而非自动迁移；
  数据保留，安全的诊断/恢复流程仍待制定。
  首次 submit、执行门和 flush 会先恢复；恢复前只读查询返回
  UNINITIALIZED，不假称 NONE。TIMEOUT/REJECTED 跨重启保持；
  重复 restore 不回滚已初始化的内存状态；UNKNOWN 可显式重读以便
  后端暂时故障恢复，但不能通过普通 submit 或 flush 覆盖，
  持久数据损坏后的维护恢复方式待定。
  epoch 耗尽后同样需要维护方案；不得通过重置计数让旧确认重新有效。
  真机证据：r12-validation A-4 节（v1 时期）；v2 由 RF1 native_sim
  双启动复验。

## 未测与边界

SDK 工作区独立版本化/隔离与外发保障；试用数据晋升（D05 细节）；
多候选（D05=单候选已批）；当前源码的设备/Forge 字节对拍真机复验。
RF1 残余：trial_end 后槽内字节不受试用治理（稳定集准入=D05 待定）；
digest 计算与模块装载之间存在单线程 TOCTOU 窗口（同线程无并发写槽，
多线程写槽须另行加锁）；`sha256_stream` 全局上下文仅单线程。
调用方仍须在合适时机显式 flush：尚未持久化的试用状态在断电后不能
恢复。暂存写入发生在试用候选提交之前的窗口及稳定集准入仍待 D05
完整交付流程约束，不能声称所有包已强制经过用户确认。
