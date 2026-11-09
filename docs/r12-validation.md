# R12 工厂/试用/晋升验证

授权边界：固件面（试用状态机+工厂 intake 契约）；**SDK 未安装/未接入**
（7.1 推进顺序——实际工厂接线待 owner 获批任务）；持久试用状态（跨
重启）随板级批（R03 存储端口消费）。

## 验收矩阵与真实结果（2026-11-08；delivery 套件 3/3 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 唯一晋升路径 | 确认前 end=STATE（不生效）；confirm 后 ACTIVE | test_lifecycle_and_user_gate |
| 内容绑定 | digest/permissions 随候选保存（确认对象可读） | 同上 |
| 单候选 | ACTIVE 期 submit=BUSY；end 清槽 | 同上 |
| 拒绝/超时 | REJECTED 槽保留可重推；TIMEOUT 后 confirm=STATE | test_reject_and_timeout_paths |
| intake fail-closed | 未知 payload/预算域外全拒；Lua kind=1 合法 | test_factory_intake_gates |
| 工程≠用户验收 | intake 通过只到 PENDING | 同上 |
| 回归 | 主机 97+CTest 6/6+架构 57 源 | 本轮实跑 |

## 自行决策（建议值——已追认：owner 2026-11-09 整体追认，计划 §10.3）

① intake 预算域=Lua 工厂合同同域（1e7/512KB）；② REJECTED/TIMEOUT
均保留槽可重推（与 D05"槽保留可重推"语义一致）；③ 试用数据隔离=
候选槽独立（试用数据晋升归 D05 细节+板级持久面）。

## 未测与边界（如实）

OpenHands SDK 实际接入（版本/隔离/外发/费用/取消恢复——获批工厂
任务）；持久试用状态跨重启；设备/Forge 字节对拍（真机批）；多候选
（D05=单候选已批方向）。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/delivery \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r12-tdelivery/zephyr/zephyr.exe
```

## 板级批 A-4（2026-11-09）：试用状态跨重启持久

- `entelea_trial_flush/restore`（meta 单槽；记录 38B：ver/state/
  version/permissions/digest）。恢复 **fail-closed**：无记录/坏记录
  （CRC/版本/域外 state）→ NONE 不复活垃圾；TIMEOUT/REJECTED 约束态
  跨重启保持（confirm 仍拒）；清槽持久。
- delivery 套件新增 test_persistence_roundtrip_and_fail_closed
  （往返/约束语义不放宽/坏记录/清槽）。
- **真机证据（hw_flash_persist A-4 相位）**：P1 submit+flush →
  生产 SW 复位 → P2 restore=PENDING + 候选内容绑定（digest/
  permissions/version）跨复位保持——`flash-persist-a4.log` 0 FAIL。
- 磨损注记：flush 时机归装配层（试用事件稀疏——meta 乒乓可接受；
  产品磨损策略随 D05 细节）。

## D 批（2026-11-09）：最小工厂闭环（SDK→Forge→设备）

owner 授权 §10.4-D1/D2（SDK 接入+真云付费调用）执行记录。

### D-1 SDK 栈（已交付）

- venv：`/home/emb/project/workspace/openhands-venv`（Python
  **3.12.13**，uv 管理独立安装；不触碰冻结 .venv）。Python 3.14
  实测 pip 解析器对该依赖树回溯 >20min 不可用——3.12+uv 秒级解析。
- pin：openhands-sdk==**1.60.2** + openhands-tools（terminal/
  file_editor）+ PyNaCl 1.6.2——`build-support/factory-tools.json`。
- LLM 链路冒烟：SDK LLM.completion → litellm `openai/` 前缀 +
  base_url → router（glm-5.3-flash）content='ok'（台账已记）。

### D-2 Forge 工作区（已交付）

`/home/emb/project/workspace/forge`（两仓库之外）：章程 README
（边界：不动两仓库/密钥纪律/凭据只经 0600 json/额度台账）+
`packer/tsap_pack.py`（TSAP v2+COSE_Sign1 ed25519 打包签名——与
设备 R05 验签管线字节对齐；产 .tsap/factory_pkg.h/candidate.json）
+ `packer/selftest.py`（**独立复刻设备 verify 逻辑**做字节级自证：
头/摘要/COSE 精确子集/内嵌副本一致/ed25519——不共享构造代码路径）
+ `tasks/factory-brief.md`（SDK 任务书）。

### D-3 最小工厂闭环（终态：双闸 11/11 PASS）

链：SDK 产 wasm → tsap_pack 签名 → 设备闸 `hw_factory_intake`
（新 app）：

1. **工厂 provisioning**：`entelea_prov_write_maint`（生产维护通道：
   校验先行+per-boot 一次窗）注入工厂根 pk0 → prov_load 装载 ✓
2. stage（begin+512B chunks）→ 3. `pkg_verify`（显式工厂根）✓
4. **D-4 设备半边对拍**：slot 全量字节 sha256 == Forge
   candidate.package_digest（构建注入）✓
5. factory_intake → PENDING_CONFIRM ✓ → 6. confirm → ACTIVE ✓
7. wasm load（内部复核验签=默认信任集=prov pk0——与步骤 0 闭环）✓
8. run/spin 调用 → 9. **hang 真死循环 → E_TIMEOUT 有界停止+实例
   销毁**（R-V1 夹具——替代 wasm 测试"预算 0"技巧证据）
10. trial_end+flush 清槽收尾（无人值守安全态）。

闸失败纪律：任一 CHK 失败 → system_fail(FAIL_BOOT(7))（fail-safe
循环——不假 PASS）。

### SDK 迭代记录（工厂产物质量——如实）

- run001（84B，~1M token 估）：verify/digest/intake/confirm 全过；
  wasm load 拒（spin br_if 分支栈型不匹配）。
- run002（94B，1.02M token=¥4.10）：仍拒（hang 尾缺 i32.const 0
  ——wasm 验证器静态栈型要求）；另 run 语义 3N+1≠2N+1、spin 回边
  不减计数（闸 N=0 面不可判别——wasm_call argv 硬编码 0 限制，
  如实注记）。
- run003（三缺陷反馈+逐函数 wat 模板）：30min 未交付终止（usage 估
  ~0.5M）；**产物线转 forge `packer/wasm_gen.py` 确定性组装**（R12
  合同"Forge 掌握产物约束"——LLM 产码→确定性组装→签名；wabt 不入
  依赖）。组装器自身亦经闸反例修正一处 opcode（0x4E=ge_s 误作
  gt_s——0≥0 恒真回边=死循环，闸 E_TIMEOUT 判定面捕获）。
- **终态（run003 终产物 91B）：native_sim + ESP32-P4 真机双闸
  11/11 PASS**——provisioning/stage/verify/digest 对拍/intake/
  PENDING/confirm→ACTIVE/load/run/spin/**hang 真死循环 E_TIMEOUT
  有界停止（R-V1）**/end+flush/"factory loop closed"。证据：
  build/d3-gate-{nsim,board}/gate-factory-loop.log。
- 板级教训（真机实测）：①main 栈 8K（pkg_verify+tweetnacl 深栈
  ——默认栈溢出=PC 落 z_main_stack Illegal instruction）；
  ②WAMR target=RISCV32_ILP32F（rv32imafc ABI ilp32f）。
- **闸即主机校验器**：SDK 产物两轮被设备管线拦截=闭环防线工作的
  证据；额度台账逐笔（~2M/18M 工厂软分配）。

### 复现（D 批闸——gate_run.sh 两相；工件=forge artifacts/run003）

```sh
# native_sim sim-flash 全链
RUN=run003 bash /home/emb/project/workspace/forge/gate/gate_run.sh nsim
# 板级（ESP32-P4）：构建（-DFACTORY_PKG_HEADER 默认=run003）+烧录+监听
RUN=run003 bash /home/emb/project/workspace/forge/gate/gate_run.sh board
# 证据：build/d3-gate-{nsim,board}/gate-factory-loop.log
```

### E-2 关联（wasm 错误面收窄——B 批 review P3 项）

`wasm_call` 异常面收窄：配额到期（WAMR 异常串 "instruction
limit"）=E_TIMEOUT；其余 trap=E_IO——实例两者皆销毁。新增
test_trap_vs_quota_error_split（unreachable 模块→E_IO ≠ 配额 0
→E_TIMEOUT 判别对照）3/3 绿。
