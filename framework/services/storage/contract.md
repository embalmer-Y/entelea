# R03 存储合同（子闭环 1：端口 / meta / noinit）

迁自旧 ts-store 的 part/meta/noinit 子集；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## 分区端口

- 每个分区是**隔离命名空间**（ENTELEA_PART_*，编号只追加）；后端不可知
  （ops 表为模块私有）。越界（含 off+len u32 回绕）与未启用分区显式
  `E_RANGE`，不静默截断。
- 本子闭环仅 RAM 后端（native_sim/主机测试；0xFF 抹除语义，test_reset
  模拟掉电后全新镜像）。flash 后端（DT fixed-partitions + 写对齐垫片）
  与宿主文件后端随板级批迁移——**不宣称持久介质已验证**。
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

## 未测与边界

RAM 后端为进程内隔离面，不证明 flash 持久性/掉电半写（板级闸 D12）；
`noinit_put` 失败不可返回（静默丢弃，读取面如实无留痕）；flash 上下文
可写性（system_fail 语境）随板级后端复核。
