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
| 容量边界 | 满 8 槽+第 9 删无覆盖=E_NOMEM 显式拒且零复活；覆盖式复用成功 | test_tombstone_capacity_no_revival |
| 回归 | 主机 97+CTest 6/6+架构 53 源 | 本轮实跑 |

## 自行决策（建议值，待追认）

① 链=64/条目=128（有界建议值）；② 删除=**累计墓碑列表**（8 槽有界；
review P1 修复：单一墓碑被第二次 delete 覆盖会复活首区间——已改累计
列表；计划符合性审查 P2 追加淘汰纪律：只复用被新区间**完全覆盖**的槽，
满且无覆盖=E_NOMEM 显式拒——绝不覆盖仍有效的删除区间；全区间删除
恒覆盖既有一切=永不因满而失败）；③ 同意=布尔参数（同意粒度/撤回
语义随 D06）。

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
