# R10 经历记忆验证

授权边界：机制面（RAM 链）；持久化/断电恢复/巩固随板级批（R03 存储
端口消费）；D06/D07 最低特性/后端成本=真机决策。

## 验收矩阵与真实结果（2026-11-08；memory 套件 3/3 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 同意门 | 无同意 E_PERM 且零写入（count=0） | test_write_consent_and_bounds |
| 参数 | NULL/len=0/超 128 全拒 | 同上 |
| 有界 | 69 写=64 存活+5 让位（序保留） | test_bounded_chain_eviction |
| 删除 | 墓碑区间跳过（10/30 存活）；逆区间拒 | test_delete_tombstone_no_revival |
| 不复活 | 墓碑外新写正常（按时间戳判） | 同上 |
| 回归 | 主机 97+CTest 6/6+架构 53 源 | 本轮实跑 |

## 自行决策（建议值，待追认）

① 链=64/条目=128（有界建议值）；② 删除=单一活动墓碑区间（新 delete
覆盖旧——简化面；多区间归 D06 最低特性定案）；③ 同意=布尔参数（同意
粒度/撤回语义随 D06）。

## 未测与边界（如实）

持久化/断电恢复/巩固事务（板级批）；"不记住"权面（D06）；跨设备
同步（不在 R10）。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/memory \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r10-tmemory/zephyr/zephyr.exe
```
