# R03 配置合同（provisioning）

迁自旧 ts-store prov 面；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## 装载与只读

- PROV 分区：u32 cbor_len | u16 crc16 | CBOR（小端头）。装载失败
  （CRC/schema/域外）= `E_IO`，调用方 fail-safe；**运行时无写接口**
  （prov.c 零写调用=机械检查目标；写=维护窗或烧录注入，分文件实现）。
- 未装载/失败兜底 = v1 等义安全侧（orchestrated + estop 远程清除开放）。
- schema v1（9 对定序）与 v2（11/12/15 对，键序=v1 前缀+尾部追加）；
  确定性子集解码，超集/不定长/键序错/域外值/键在值空 = `E_IO`
  （fail-closed，不静默归并）。缺字段缺省：deploy_mode=orchestrated、
  estop_remote_clear=1、lconf=0、TLS off（存量 prov 零重刷兼容）。
- TLS 三元组（≤768/768/256 DER）三键同现同缺；R03 起随装载态常驻
  （旧 CONFIG 门控在 R06 消费方接入时复核足迹）。

## 维护写通道（一次性窗）

- `write_maint`：先校验（与 load 同一解析路径）后写；失败零写入；
  per-boot 一次性（RAM 旗）——再写须复位循环，远程换身份可观测限速。
- estop 边沿消费 prov（旧 estop_init 惰性装载面）随 D12-⑤ estop 决策
  落地，本子闭环 estop 边沿仍为固定缺省上升（记录于 R02 合同）。

## 未测与边界

真实烧录工具链/远程通道（R06/R12 消费方）；RAM 后端不证明 flash
程序一次纪律；域外值矩阵以旧 oracle 为准迁移（见验证记录）。
