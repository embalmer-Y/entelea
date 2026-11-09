# R03 存储合同（子闭环 1：端口 / meta / noinit）

迁自旧 ts-store 的 part/meta/noinit 子集；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## 分区端口

- 每个分区是**隔离命名空间**（ENTELEA_PART_*，编号只追加）；后端不可知
  （ops 表为模块私有）。越界（含 off+len u32 回绕）与未启用分区显式
  `E_RANGE`，不静默截断。
- 后端=可挂载（板级批 2026-11-09 交付）：公共 ops 类型+
  `entelea_storage_mount`（默认 RAM 模板；`ENTELEA_NO_RAM_BACKEND`
  惰性档省 540KB SRAM）；platform flash_store（flash_map 布局映射/
  META 双副本分扇区重映射/写块垫片/超集 erase_off）。挂载失败（配置
  了 flash 而不可用）=boot 步骤 fail-safe；无 flash 面（弱桩
  E_UNINIT）=RAM 直通。RAM 后端 0xFF 抹除语义不变（test_reset 模拟
  掉电后全新镜像）。**meta 单槽唯一消费者=R12 trial；多消费者小状态
  走 KV（app_hash 键空间——pkg active 已迁入，板级批 review P1-1）**。
  宿主文件后端仍延后（D02 环境）。
- 后续子闭环按枚举尾部追加：PROV（配置）、AUDIT（审计）、KV（App 状态）、
  TRUST（信任材料）；SLOT 归 R05 包事实批次。

## 掉电安全 meta

- 双副本 + 单调序号 + CRC16-CCITT；活动 = 序号较大且 CRC 通过者。
- 写序：范围抹除非活动副本 → 写 → **回读校验** → 成为新活动（撕裂时
  旧副本仍完整）。单副本损毁可恢复；**双副本皆损 = `E_IO`**（调用方按
  缺省安全态/fail-safe，不自动"修复"、不伪造空数据）。
- 读路径双副本独立缓冲（旧 MD1.1b 缺陷回归保留：共用缓冲在 copy0 较新
  时读回旧副本内容）。

## noinit 留痕（一次性）

- 布局：magic|len|crc16|rec（小端）。fresh=true = 无/损坏（按无留痕
  呈现）；有效记录 fresh=false；**读后抹除**（one-shot，防陈旧留痕跨
  复位误报）。超长/空输入拒绝落盘。
- **R02 遗留接线**：`system_fail` 停机前经 `entelea_safety_record_fail_
  reason`（safety 弱桩，本模块强实现）落 u32 reason；boot 尾部步骤
  `storage_init` 消费留痕，装配/观测面经 `entelea_storage_last_reset`
  上报。留痕缺失（无存储构建/写失败）如实呈现，不伪造。

## 审计两页乒乓环（子闭环 2）

- 8KB = 两页 × 4KB（擦除块对齐）× 64 槽 × 64B 定长；"覆盖最旧"经乒乓
  实现：active 页写满翻页，**页首写前擦对页**——掉电保留最近 ~64 条
  （两页全满瞬态 128）。掉电半写：槽头 magic + seq 连续性断尾丢弃。
- init 扫描恢复游标（reboot 后 seq 续接）；append 前未 init = `E_STATE`。
- **落盘范围=异常类**：commit 拒绝（含全部早退路径）+ estop 沿（evt=1，
  系统事件 ch_idx=0xFFFF，不节流）。正常输出流不落盘（热路径磨损）；
  全量内存环保留归 R04 观测面。
- 节流：首条立即、其后 1Hz 采样（旧 F1/DEC-52①：失败提交循环的写/擦
  风暴防护），丢弃计数 `entelea_safety_audit_throttled()` 可观；persist
  失败不反噬提交路径（观测面降级）。时间域=模块时钟（未绑定/0 域仅
  boot 前配置窗口——生产面 boot 先绑钟）。

## 信任根表（子闭环 4）

- TUF root 同型全量替换：`map{"ver":uint≥1, "keys":[bstr(32)×0..8]}` 恰
  2 对定序键；撤销=次版剔除（无独立 revoked 列表）；超上限/键内重复/
  ver=0/尾部余量 = `E_IO`（fail-closed）。双损/空 = `{0,0}` 工厂根单源。
- 8KB 双副本乒乓（meta 同型撕裂安全 + 回读校验）；write = 校验先行
  （非法零写入）→ 写非活动 → 活动切换 → RAM 重载。**签名/单调 ver
  门控在 R12 编排链**（本层仅持久化——职责分层记录）。

## App 状态 KV（子闭环 4）

- 每 app 一份 ≤250B blob，按 app_hash 归属跨升级/回滚保留；**不是经历
  库**（合同既有约束）。两页 ×15 槽追加式；load 取最大 seq 匹配槽；
  页满压实翻页（每 app 最新重写，先写后切——压实窗口掉电最坏丢"压实
  开始后未重写"的 app 状态，如实接受）；半写断尾同 audit。
- 有界：至多 15 app（压实保 14 为新写留余量）；超出弃 seq 最旧 app。
  未存=`E_NOTFOUND`；容量不足=`E_RANGE`（如实）。

## 未测与边界

真断电（VBUS）半写域=到场项（真机 SW/POWERON 域持久化已证——
r03-validation 板级批节）；`noinit_put` 失败不可返回（静默丢弃，
读取面如实无留痕）；槽级尾损仅 magic/seq 断尾可检（旧格式迁移
保真——audit/kv 无槽 CRC，真撕裂归到场断电实测）。
