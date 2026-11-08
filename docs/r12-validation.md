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
