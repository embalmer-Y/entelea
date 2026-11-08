# R03 存储/配置/审计验证（进行中批次）

授权边界：按 owner「按现有重构计划顺序推进」指令开工 R03（depends_on
R01=done）。D10 未决项（数据类别/撤销/外置介质擦除与备份范围）按规则
标 blocked、机制先行；slot→R05、flash/宿主文件后端→板级（D12）。
RAM 后端 = 进程内测试隔离面，不冒充持久介质。

## 子闭环 1（提交 `681d785`）：端口 / meta / noinit / R02 留痕接线

### 验收矩阵（承诺 → 负测 → 证据）

| 承诺 | 破坏条件→负测 | 证据 |
| --- | --- | --- |
| namespace 隔离 + 越界拒绝（含 u32 回绕） | off=0xFFFFFFF0+len 越界、len 越界、未启用分区 → 全 `E_RANGE` | store 套件 `test_meta_params_and_bounds`（探针直通后端） |
| meta 撕裂恢复 | 破坏活动副本→另一副本接管；双损→`E_IO`（不自动修复）；撕裂后写恢复 | `test_meta_roundtrip_and_tear`（6 轮往返含 copy0 较新路径——旧 MD1.1b 回归保留；双损后单副本存活继续写） |
| meta 写校验 | 写后回读不符→`E_IO`（实现面；RAM 后端不触发半写，flash 语义随板级） | 代码路径+合同；未测=flash 半写 |
| noinit one-shot | 二次读=fresh；超长拒绝落盘；空=fresh | `test_noinit_semantics` |
| system_fail 留痕闭环 | 原因编码往返（FAIL_BOOT(2)/FAIL_WDT(0)）；init 消费前不呈现、消费后呈现 | `test_system_fail_reason_persisted` |
| 启动顺序=规格（尾部追加） | 表 5 步守卫 | core `test_boot_step_order_is_spec` + store `test_boot_table_has_storage_step` |
| 留痕缺失如实呈现 | 无存储构建=弱桩（不伪造）；noinit_put 失败静默丢弃由读取面呈现 | 代码路径+合同（未测=无存储链接变体） |

### 实际执行（2026-11-08，本机）

- native_sim 经新入口：store 套件 5/5 SUCCESSFUL（每次 configure 过
  `[effective]` 生效比对）；R02 回归 9 目标全部 SUCCESSFUL 零失败
  （boot 表变 5 步后 core/safety×2/wdt×2×2/minimal 重跑通过）。
- 主机：架构检查 24 源恰分类（framework/services/ 纳入框架组）；
  Python 94 项 OK；CTest 6/6；台账 800 项源核验。

### 未测/边界（如实）

flash/宿主文件持久性、真实掉电半写、system_fail 语境 flash 可写性——
板级（D12）；prov/audit/kv/trust 子闭环未开始；250B KV 不作经历库
（合同既有约束）；无存储链接变体（弱桩路径）未构建验证。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/store \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r03-tstore/zephyr/zephyr.exe
```
