# R11 模拟智能/情感/适应验证

授权边界：机制面（确定性回放可证机制——不冒充真实数月体验）；
D06/D12 长期阈值=真机决策。

## 验收矩阵与真实结果（2026-11-08；affect 套件 3/3 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 单写者+参数 | dt=0/NULL 拒 | test_tick_evolution_and_determinism |
| 确定性演进 | 升/降/回中位；同输入重放一致（memcmp） | 同上 |
| 有界变化 | 步长饱和 10；0/100 钳制 | 同上 |
| 版本化 | 回滚后新提交=新版本（不复用 v2）；环满让最旧 | test_adapt_versions_and_rollback |
| 回滚 | 无历史 E_STATE 不静默；回滚恢复旧参数 | 同上 |
| 独立性 | 情感关闭回中位；适应独立于情感状态 | test_emotion_off_independent |
| 回归 | 主机 97+CTest 6/6+架构 55 源 | 本轮实跑 |

## 自行决策（建议值，待追认）

① 维度=4/环=8（有界建议值）；② 演进规则=升/降/回中位（50），步长
=dt/1000 饱和 10；③ 回滚不删经历（R10 独立）。

## 未测与边界（如实）

真实数月轨迹（真机批）；"适应试图改授权"负测=结构性不存在（无写
路径——代码审查面）；产品参数语义（产品批）。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/affect \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r11-taffect/zephyr/zephyr.exe
```

## 计划符合性审查补记（2026-11-08，轨道 1）

- 如实点名（处置归计划修订/轨道 2）：计划 L401 输入资产
  A-AFFECT/A-REFERENCE 仍 not_migrated 且此前验证记录未声明；
  计划 L406-407 之"角色连续性"正测与"模型输出/来源丢失/反复恢复"
  负测未交付亦未逐项归属（异常时间已按层归属 R01 的做法正确）。
