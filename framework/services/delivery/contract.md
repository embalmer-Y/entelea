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

- 候选 = digest（设备计算：验签后整文件 sha256，同 Forge `package_digest`）
  + 权限列表（分号分隔的 `class:op:instances` 文法，≤96B，空 = 无权限）
  + 步数（1..1e8）+ version + **slot** + **epoch**。宿主只申请权限、
  步数、版本与槽，**不能指定摘要**（F4）。NONE → PENDING_CONFIRM：
  `submit(req)` 先校验字段（槽域/权限文法 E_PARAM，步数 E_RANGE），再
  检查状态（BUSY/E_RANGE），再验签槽内包并计算摘要（无有效包 = 验签错误
  原样返回，不改状态），最后分配 epoch（非 0、单调、跨 end/restore 不
  回退；到 `UINT32_MAX` 后返回 `E_RANGE`，不能回绕复用旧确认）；
  PENDING → ACTIVE（`confirm(&shown)`——开启试用前须由可信调用方取得真实
  用户确认；API 本身只比较对象，不验证用户身份或交互；shown 须与当前候选
  摘要、权限列表、步数、version、slot、epoch 全部一致，否则 E_PERM 且保持
  PENDING）/ REJECTED（reject）/ TIMEOUT（fail-closed 撤销）；ACTIVE → NONE
  （end）。ACTIVE 是试用状态，不代表稳定集晋升。
- 单候选与槽保留：ACTIVE/PENDING 期 submit=E_BUSY；REJECTED/TIMEOUT
  只可同槽重推（他槽=E_BUSY）；slot 域外=E_PARAM；非预期迁移=E_STATE。
- **执行门（RF1）**：`entelea_trial_gate(slot, digest)` 由 wasm 宿主在
  装载（卸旧之前）与每次调用时查询：候选槽（或恢复后的 UNKNOWN 槽）
  仅 ACTIVE 且槽内当前字节 digest 相符=OK，其余（PENDING/REJECTED/
  TIMEOUT/确认后换字节）=E_PERM。NONE 或非候选槽=OK（不受试用治理）。
- **授权（RF1 F1，D-A/B/C/D）**：`entelea_trial_grant(slot, digest, out)`
  仅在 ACTIVE 且槽与字节相符时授予确认的权限列表与步数，不相符一律
  E_PERM 且输出清零；wasm 宿主每次调用重新查询（不缓存）。natives 授权
  = 确认的权限列表 ∩ 宿主注入的 principal 授权，任一缺失即拒绝。未经
  确认的包（NONE/非候选槽）无任何 natives 权限（A1）；纯计算仍可执行。
  执行配额 = min(调用方请求, 确认步数, 设备上限 1e8)。签名不覆盖权限与
  步数（D-D：首期由用户确认与持久记录约束）。
- **工程通过≠用户验收**：intake/验签通过只到 PENDING——confirm 之前
  该槽不可执行。产品构建无自动确认；`FACTORY_DEV_AUTOCONFIRM` 仅为
  开发夹具，无用户验收效力。

## 工厂 intake（fail-closed）

- 未知 payload_kind 拒（不默认解释为 wasm/不按后缀推断权限——
  R05 同纪律）；预算域外拒（0<budget≤1e7、0<mem≤512KB——与
  Lua 工厂合同同域）；权限列表超长拒。
- intake 与 submit 共用设备验签与摘要计算：声明的 `package_digest` 必须
  与设备计算的整文件摘要相等，否则 E_INVALID_SIG 且不改变状态。通过后
  只到 PENDING。

## 跨重启持久（板级批 A-4 已交付）

- `flush/restore` 经 R03 meta 单槽（本层独占——pkg active 已迁 KV，
  review P1-1）。v3 记录 141B 小端：ver=3|state|cand_version|slot|
  last_epoch32|budget_steps32|perm_len|perm[96]（零填充）|digest[32]。
  v1/v2（旧布局）按坏记录处置，不迁移。
- 恢复 fail-closed（RF1）：长度/版本/state 越界、非 NONE 时 slot≥2、
  epoch=0、步数域外或权限列表文法坏 → **E_IO、TIMEOUT + slot UNKNOWN**
  （两槽门拒）。
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
RF1 残余（未闭合，见 [RF1 复核](../../../docs/rf1-validation.md)）：稳定集
准入（D-E，WP3）未实施——`trial_end` 后槽内字节的纯计算仍可执行（natives
无授权）；非候选槽的稳定记录与晋升流程待实现。装载门的摘要与装载字节由同
一趟捕获读取比对（F3）；验签读取与摘要读取之间仍依赖串行存储假设（同线程
无并发写槽；多线程写槽须另行加锁，未证明）。`sha256_stream` 全局上下文仅
单线程。调用方仍须在合适时机显式 flush：尚未持久化的试用状态在断电后不能
恢复。
测试接缝：`entelea_trial_submit_with_digest_for_test` 仅 ENTELEA_TEST 构建可
用，状态机用例经此接缝提交调用方摘要；生产路径的设备摘要由 wasm 套件用真实
签名包验证。
