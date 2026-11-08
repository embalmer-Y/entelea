# R08 最小辅助闭环验证

授权边界：R08 depends_on R03/R04/R05/R06（软件面已交付）；真实云模型
调用/延迟/超时未测（D08 真机+云批——不碰真实云）。

## 交付范围

- 有界上下文窗（16 建议值；溢出弃最旧+dropped 计数）。
- 触发纯判定（窗满/显式——无内部定时器；周期阈值随 D08）。
- 结构化候选 + 执行门（R04 权限 → R02 保护层唯一写路径；模型=建议者
  不能自批）。
- 云依赖面：assistant_available = model∩net UP；不可用停智能、独立
  App 照常（R06 独立判定复用）。

## 验收矩阵与真实结果（2026-11-08；assist 套件 4/4 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 有界窗 | 21 条入窗=16 存活+5 弃最旧（序保留） | test_context_window_bounds |
| 触发 | 差一条不触发；窗满触发（纯函数） | test_trigger_on_full_window |
| 模型不能自批 | 未授权 principal → E_PERM 且零输出（readback 不变） | test_candidate_gate_and_protection |
| 保护层全链 | LINKLOSS 期拒；estop 锁存期拒；授权写经 commit 落值 | 同上 |
| 云停用 | 初始/model down/net 断 = 停；双 UP=可用 | test_cloud_down_stops_brain_only |
| 独立 App | storage 独立恢复不改变智脑停用 | 同上 |
| 回归 | 主机 97+CTest 6/6+架构 49 源 | 本轮实跑 |

## 自行决策（建议值，待追认）

① 窗=16/候选=4（有界建议值）；② 触发=窗满/显式（周期随 D08 真机）；
③ OUTPUT 权限门=目标类 write 声明（完整 periph 绑定归 R09+ 装配）。

## 未测与边界（如实）

真实模型调用/恶意输出矩阵/超时（真机+云批）；多轮对话/记忆整合
（R10）；媒体候选（R09）；触发周期阈值（D08 校准）。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/assist \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r08-tassist/zephyr/zephyr.exe
```
