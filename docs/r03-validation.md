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

## 子闭环 2（审计环 + safety 异常落盘）

- store 套件 7/7 SUCCESSFUL：翻页边界（65 过渡态/恰满 128/翻页后
  66+最旧段被覆盖）、reboot 游标续接（seq 131）、**半写断尾**（破坏
  槽0 → 仅另一页 64 条连续段）、未 init append 拒绝、**commit 全早退
  路径落审计**（外层包装）、节流（同窗第二拒=丢弃计数1、环不增长）、
  estop 沿 evt=1（补发面直调，正向控制）。
- 回归：minimal/core/safety×2/wdt_start/wdt_hardware 全部 SUCCESSFUL
  零失败；主机 94 项 + CTest 6/6 + 架构 25 源。
- 自行决策（建议值——已追认 owner 2026-11-09，计划 §10.3）：审计节流窗沿用旧 1Hz（DEC-52① 出处）；
  boot 打印留平台层（存储模块 printk-free）。

## 子闭环 3（prov 配置面）

- store 套件 10/10 SUCCESSFUL：v1 全字段+缺省（routers 余位清零/orch
  estrated/远程清除开放/lconf 0/TLS off）；v2 部署模式显式值；域外值
  三面 fail-closed（mode/clear/lconf=2）；v/pairs 错配；**尾部余量拒**
  （本轮新增收紧：定长 schema 精确消费，walk 末 rem!=0=E_IO）；CRC 破坏
  →E_IO；lconf/TLS 15 对长度镜像（装载态 ca_len 等）；维护窗（非法零
  写入→分区仍空 load E_IO、合法装载、一次性窗 E_STATE、复位模拟再写）。
- 自行决策（建议值——已追认 owner 2026-11-09，计划 §10.3）：① TLS 三元组常驻装载态（旧
  CONFIG_TS_NET_TLS 门控待 R06 消费方复核足迹）；② 尾部余量收紧为拒
  绝（旧 prov 未显式测，trust 面已有同语义——统一采纳）；③ estop 边沿
  消费 prov 仍固定缺省上升（随 D12-⑤ 决策落地）。

## 子闭环 4（信任表 + App KV）

- store 套件 **12/12 SUCCESSFUL**（累计 25 用例断言组）：
  - trust：初始空=工厂根哨兵；schema 矩阵（ver=0/超 8 键/键内重复/
    尾部余量全拒）；写入往返（键字节差 0x10/0x14）；坏表零写入；
    次版乒乓；撕裂回退旧副本；双损回哨兵。
  - kv：未存 NOTFOUND/参数负测；3 app 往返+隔离；覆盖写取最新；
    容量不足 E_RANGE；14 轮双 app 写触发压实翻页；复位重扫描后
    最新保留与 app 存活。
- 自行决策（建议值——已追认 owner 2026-11-09，计划 §10.3）：trust/kv 直迁旧机制（A-STORE 五子闭环
  全部落地，组级 migrated；验证面按整组 oracle 判——store 套件为组级
  验收证据，状态 not_run 保持与 R01 先例一致直至板级持久化复验）。

## 板级复核标记（计划符合性审查 P3，flash 批必查）

audit/kv 槽体无 CRC（撕裂探测仅 magic+seq 断尾——与旧机制一致）；
RAM 后端无损语义下不可达，**flash 后端接入时须复核槽内尾损可检性**。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/store \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r03-tstore/zephyr/zephyr.exe
```
