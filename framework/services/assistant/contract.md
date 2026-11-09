# R08 最小辅助闭环合同

新-only（N-CONTEXT/N-DELIBERATION）；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## 上下文汇聚

- 有界窗（16 条建议值）；窗满**弃最旧+dropped 计数**（不阻塞发布方、
  不无界增长）。快照只读（观测面）。事件 payload 借用发布方生存期
  （R02 事件合同——静态存储约束）。

## 触发

- 建议值：**窗满或显式**触发（无内部定时器——确定性；周期触发随
  D08 真机校准）。`should_deliberate` 为纯判定（无副作用）。

## 候选（模型=建议者）

- 结构化候选：kind（NONE/OUTPUT/TASK/NOTIFY）+ 人读 text + 来源
  （source=MODEL——建议非观测）+ 目标/值。
- **模型不能自批**：候选必须经 R04 principal 裁决（E_PERM 可观测
  拒绝）+ R02 保护层唯一写路径。伪造 principal 不可达（R01 合同）。

## 执行门

- OUTPUT：权限门（gpio/pwm write 声明）→ `entelea_safety_commit`
  （限幅/slew/锁存/审计全链复用——本层零旁路）。
- TASK：R04 任务面（上层组装——本层不重复受理逻辑）。
- NOTIFY：无副作用（无执行回执面）。

## 云依赖（D08）

- `assistant_available` = model UP 且 network UP（云模型经网络到达）。
  不可用 → 停用智能（上层据此停触发）；**独立 App 照常**（各自查
  各自依赖——R06 独立判定面）。不伪造本地离线智能。

## 模型适配层（B-2 已交付）

- 策略层（框架）：`entelea_model_ask`——依赖联动（模型+网络任一
  DOWN=E_STATE 且**不触传输**，D08）；有界重试（仅瞬态 E_IO，封顶
  ENTELEA_MODEL_RETRIES=2；E_TIMEOUT/E_RANGE 不自动重试——成本/语义
  不可知归调用方）；用量累计（预算/台账观测）；输出缓冲下限
  （ENTELEA_MODEL_OUT_MIN=64——推理模型实测先耗 reasoning tokens，
  过小必截断）。失败尝试若传输报用量亦计入（成本如实）；依赖门
  逐尝试重查（重试中途 DOWN=E_STATE 停触传输）。
- 传输层=可插拔 ops（mock 测试闭环；真传输随真网批/SDK 批接入同一
  形状——框架不含网络栈/TLS，无新依赖）。主机真调用冒烟：
  tools/model_smoke.py（凭据在工作区外，台账逐笔记）。

## 未测与边界

真实模型调用/延迟/恶意输出矩阵（真机+云批）；周期触发阈值（D08）；
多轮对话/记忆整合（R10 经历面）；模型超时（D08 真机）。
