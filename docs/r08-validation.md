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

## 自行决策（建议值——已追认：owner 2026-11-09 整体追认，计划 §10.3）

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

## 计划符合性审查补记（2026-11-08，轨道 1）

- 新增 test_output_domain_gates：跨类拒（双向）/域外 value_kind/
  非 DEV_OUTPUT 目标拒（修复：类检原条件化在 safety_uid!=NULL 内，
  传感器类描述符 READ 即 acquire 可跳类检）/POWER 路由（无
  power:write=E_PERM 零输出；有权限+预算内经 power_request 落值）
  ——review P1 修复三分支零负测的证据缺口消除。
- ~~计划步骤“Task 执行+来源化反馈”未实现~~——B-3 已交付销项（下文）。

## B 批（2026-11-09）

- **B-2 模型适配层**：framework/services/assistant/model.c+model.h
  （策略层——依赖联动/有界 E_IO 重试/用量累计/输出下限）；assist
  套件 test_model_adapter_policy（联动门零调用/重试封顶/超时不重试/
  用量累计/参数域）7 用例全绿。主机真调用：tools/model_smoke.py
  （glm-5.3-flash 46t content='ok'/finish=stop/4.7s——台账已记）。
  推理模型特性（先耗 reasoning）入合同与输出下限语义。
- **B-3 任务受理面（销 R08 旧欠条）**：assistant ACT_TASK 接线 R04
  任务机（幂等键=source.id 低 16 位显式截窄——review P2-1；deadline=
  value_u 透传受理门；task_ret 前导置 0 仅成功写 id；反馈=effect 域）。
  test_task_acceptance_wiring（受理/重复 E_BUSY/反馈/无名拒/deadline
  不可知 E_STATE 透传）。

## F 批（2026-11-09）：模型真传输（Linux host 面）

### F-2 真云闸（已交付：HOST CLOUD PASS）

- `platform/linux/https_chat.c`：`entelea_model_transport_t.chat` 的
  **真实现**——mbedtls 4.1.1（pin 同 Zephyr 模块树，GEN_FILES=OFF
  绕开发脚本子模块）+TLS1.3（DigiCert 链验证 REQUIRED+hostname）+
  HTTP/1.1 POST chat/completions+**值感知**最小 JSON 键查找（模型
  可在 reasoning 文本里写字面 `"content":`——纯 strstr 会命中字符串
  值内部；引号段+冒号结构位识别。实测教训：该误配曾致长 reasoning
  被当作 content→显式截断拒）。usage prompt/completion 提取；状态码
  首行解析；401/403→E_PERM、429→E_BUSY（不自动重试——额度纪律）；
  1s 量子+整体 deadline 超时界。
  TLS1.3 NewSessionTicket 后握手消息显式续读。
- `platform/linux/cloud_main.c`+`build-support/build-f2-cloud.sh`：
  凭据运行时读 0600 json（不入二进制/日志——打印仅长度）；链：
  model_bind → dep NET/MODEL 双 good×2（防抖）→ ready → **真调用
  router** → out='ok'/usage 断言 → 失联对照（NET bad → ask=E_STATE
  且 touched 计数不变=**不触传输**）→ 1 次 good=DEGRADED 中间态 →
  双 good 恢复 UP。
- 实测：`HOST CLOUD PASS`（两轮：usage 17+46 / 17+59；台账已记）。
- 诚实边界：host 面兑现 B-2 合同"真传输接入同一形状"；**MCU 面
  BLOCKED 于 C5**（r06-validation F 批节——到场项）。E_IO 重试 3 次
  面在调试轮实测（read ticket 误判期）；E_RANGE（无 content）不重试
  面同轮实测。
