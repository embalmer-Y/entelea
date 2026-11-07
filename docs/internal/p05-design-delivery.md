# P05 设计建议：工程交付、配置与恢复

建议状态与决策入口见[总览](p05-design-proposal.md)；
运行/任务状态见[设备运行时](p05-design-runtime.md)。
首期用户验收合同以[产品定义](p05-first-product.md)为准。

## 1. 部署与工程职责

独立工程工厂可运行在开发主机/受控服务，不要求 MCU 编译源码。
云 LLM 不可用停止设备智能；工程工厂不可用只是不能开发新能力，
已有独立 App 不因工厂离线停止。两类依赖不混同。

| 模块/域 | 独占状态 | 输入 → 输出/端口 | 不负责 |
| --- | --- | --- | --- |
| Device request broker | 工程请求 ID/版本、授权范围与可见进度 | requirement/approve/cancel → job/status | 不生成代码或用户验收 |
| Factory specification | 澄清结果、目标能力/预算/验收约束 | capability snapshot + request → bounded spec | 不编造设备接口 |
| Factory creation | 工作区、工程轮次/费用预算、源码候选 | spec + verifier feedback → candidate | 不获得部署/用户确认权 |
| Factory verification | 构建/导入导出/行为证据及适用范围 | sources/scenarios → evidence | 未运行不能报通过 |
| Factory package/sign | 确定内容、manifest、签名结果 | verified build → signed package | 私钥不向 App/模型开放 |
| Device delivery | 候选/稳定版本、提交事务与恢复 | stage/verify/trial/commit → receipts | 不相信单一工厂“通过”报告 |
| Device acceptance | 试用授权和具体内容的用户确认 | trusted user action → receipt | AI 不能自批或复用旧确认 |
| Product presentation | 用户流程/临时 UI 状态 | durable states → explain/controls | 不自行更改槽位或安装状态 |

Factory 保留当前 creation/domain/tools 分离；
设备 broker 经认证版本化协议，复用适合的现有传输，不先建设新云控制平面。
首期断联工程请求推荐明确暂停/失败并允许查询/重新提交，
不无限离线积压；取消即便工厂失联，设备也拒后到候选自动试用。
正式闭环可人工授权/接线/启动受控设施，不能靠人工改代码、打包或跳过试用
冒充 AI 能力开发已完成。确认交互入口待产品设计。

## 2. 工程移交契约

```text
Engineering request:
  job_id + requirement_revision + target/profile/ABI versions
  supported capability schemas + granted permissions
  runtime dependencies + resource/output bounds
  acceptance criteria/scenarios + privacy/export scope
  creation/deployment authorization refs + cancellation/deadline
Candidate:
  job/spec revision + package/content digest + manifest/interface versions
  build/toolchain/source revision + executed verification evidence
  target compatibility + permission/data differences + known limitations
```

权限、兼容和运行依赖分别校验；能力 snapshot 过期则设备再检查，
不把工厂开工时可用当成部署时可用。
AI 提出新权限须用户批准变更规格，不静默扩大。数据外发也有单独批准范围。
生成 App 调用受控宿主能力，不能导入私有驱动接口或反向修改框架/信任根。
首期 SDK/契约由设备框架版本导出，工厂固定对应版本，不抄文档维护另一份 ABI。

工程证据建议项级 `passed/failed/skipped/not_run` 与理由、命令、版本、
输入/产物摘要；模型自述不是证据。不可适用的物理场景说明不适用，
仿真必需场景未运行不得进入可试用候选。
现有 verifier 有 simulate 端口，但当前产品参数尚未完整接入主循环；
该接线是后续工作，不认定为现有能力。

## 3. 包、运行与验收是三个状态轴

包事实：`receiving → verified → staged`；签名或兼容失败拒绝。
实例事实：`stopped/starting/running/stopping/faulted`，受依赖与资源制约。
交付事务：

```text
candidate_ready → trial_authorized → trial_running → awaiting_acceptance
awaiting_acceptance → accepted → promoting → stable
在途 → rejected | cancelled | expired | failed
promoting 中断 → recovery_required → stable | failed
```

健康检查是工程/运行条件，不自动进入 accepted。
试用许可是受限执行授权，正式确认是特定候选的晋升授权，两者不混同。
“正式安装”不意味着现在运行或自启动；启动条件单独说明。
用户拒绝/修改：先停试用/撤权限和输出，再处理候选；改内容或权限重新验收。

确认绑定：目标设备、job/spec revision、包完整摘要、权限/数据版本、
试用实例/结果引用及确认代次。不能只确认 `app_id`/semver，
也不认为 TSAP 内部某个摘要天然覆盖全部验收内容；签名包整体身份明确。
确认由受信用户入口形成，并在设备当前状态复查；远端认证策略若改变
现有只允许本地确认的路径，需要 owner 单独批准。

## 4. 推荐槽位与试用数据方案

首期推荐**每 App 一个稳定包 + 一个候选、单交付事务**，复用双槽，
不承诺任意历史版本库。同 App 再生成候选会替换未正式候选，
不可擦掉唯一稳定包；空间不足明确拒绝。

持久事务记录区分 `stable_slot/digest`、`candidate_slot/digest`、
`trial_state/authorization`、`accepted_digest`、`promotion_generation`、
`data_schema/namespace`。这是新格式建议，不认为现有 active_slot 已包含这些。
当前 meta 只描述活动槽/回滚等；activate 会停旧实例并切 meta，
所以不能直接用它运行“稳定指针不变”的用户试用。

推荐 MCU 试用换包允许受控重启：持久准备试用计划，保留稳定事实，
本次启动选择受限候选；启动失败返回稳定配置并报告。
当前 WAMR 缓存/卸载限制使“无缝热换包”证据不足。
重启不能续未知动作；独立 App/日程按自己恢复合同处理。
跨重启试用授权默认失效，需重新明确许可；仅受控换包启动计划可消费
一次授权，不能被不断重启无限延长。具体期限/计数需产品确认。

候选数据用独立命名空间，读稳定数据只能经授权只读快照，
写入不能覆盖稳定业务状态；不允许 App 任意写共享经历库。
首期代表 App 优先不需要破坏性数据迁移。
晋升有迁移时：冻结写入 → 在新命名空间准备/校验 → 持久提交包和数据
版本关联记录 → 启动确认；发布记录前旧关联有效，之后新关联有效。
若包/数据存不同介质，不宣称跨介质原子写：用可恢复事务与参与者校验，
缺一方时停该 App 并修复，不能组合新代码与不兼容旧数据。

工程/用户确认成功后才执行晋升；晋升失败保留稳定配置和错误。
旧稳定槽在本次提交恢复完成前不得回收；下一次候选上传可能占用历史槽，
所以双槽不提供无限回退。需要回退数据时另保留旧 schema 快照或拒绝不兼容回退。
现有按 app_id 保留的 250 B KV 不提供试用隔离，必须显式演进/路由，
不能换包后直接共享旧 KV 当作新合同通过。

## 5. 掉电、失败与撤销

| 中断位置 | 恢复规则建议 |
| --- | --- |
| 上传/验签前 | 保持稳定版本，残候选不可执行，可丢弃或受控重传 |
| staged 未授权 | 不运行候选、不自动确认 |
| 试用运行中重启 | 停旧任务/输出；按一次性启动计划或重新授权，不自动续试用 |
| 用户已确认未提交 | 校验确认内容/目标和事务，再恢复提交或报告修复需要 |
| 新关联发布后未启动 | 按新关联检查启动；失败按有版本的数据回退合同恢复 |
| 槽/事务/数据介质损坏 | 不猜有效版本，不静默格式化；稳定可验证才恢复 |
| App 无法停止 | 撤新提交权，隔离资源并受控重启；不复用尚被持有缓冲 |

每个持久点测试部分写、IO 失败、丢回执和重复确认。
持久化保证取决于 Flash/TF 后端，回读不是任意掉电保证。
取消/卸载只停止未来效果，不撤销已经播放、发出的请求或物理动作。
回退可用代码不自动恢复外部世界，也不能复活已删除经历。

## 6. 开发配置与正式产品

显式开发/测试 profile 可直接部署受限候选，不每轮用户试用验收；
可提高已声明预算，但硬件资源和权限有效。
必须呈现开发标识、记录来源，不能把它写成 stable_user_accepted，
转正式产品仍走具体内容的完整验收合同。
签名验证、停止、数据授权和保护均保留；
测试签名材料只用于测试目标，不能成为正式产品信任根。
生产启动拒绝/隔离不匹配的开发配置，不静默把测试包升级为产品包。

## 7. 配置、凭据和重置生命周期

推荐分类，避免“恢复出厂”一个按钮隐式清空全部状态：

| 类别 | owner/修改入口 | 建议合同 |
| --- | --- | --- |
| 身份/绑定/信任/保护配置 | 受控维护与认证入口 | 不允许 AI/App 普通写；保留当前 provisioning/信任约束 |
| 网络/云服务凭据 | Config + secret backend | UI 只读状态/引用，凭据不入普通日志/模型上下文 |
| 音量/亮度/静音/主动交互 | Config，经用户入口 | 校验范围、版本提交，重启保留用户关闭选择 |
| 产品角色/长期倾向 | 产品约束 + Adaptation | 与安全配置分离，修改影响有解释/版本 |
| App 业务状态 | App 命名空间/版本 | 停止/卸载/回退的数据处置显式，不能直接改系统存储 |
| 经历/语义/删除记录 | Memory | 删除/修复独立授权，不被网络重置默认清除 |

改凭据先阻止依赖该身份的新请求、撤相关代次，校验并提交，再重建连接；
授权/凭据撤销使在途结果失效，不能等缓存 token 自然到期。
解除绑定撤控制租约和该主体授权，停止其待执行任务；不自动删除全部记忆。
重置建议分“网络/用户设置”“App/数据”“角色与经历”类别，明确结果后确认，
信任根或安全配置变化沿既有 owner 门控。具体删除组合尚未批准。
MCU 不默认提供安全硬件密钥存储/介质加密；存在明文暴露限制须明确，
不以 API 隐藏就声称凭据抗物理读取。备份/迁移/彻底擦除另定义。

## 8. 首期闭环验收

代表新能力 App 尚待选择，可建议专注/休息助手，但不是已批准范围。
正式验收应从产品需求入口开始，真实工厂创作/验证并返回签名候选，
设备独立检查、用户试用/反馈、确认特定版本，再正式交付。
至少有拒绝、修改重验、权限变化重确认、工厂断联/取消迟到、
试用失败保留稳定版、每提交点掉电/IO 错误和不兼容数据回退用例。
开发直部署只证明工程路径，不能替代此产品闭环。
