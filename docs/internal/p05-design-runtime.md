# P05 设计建议：设备运行时与可选扩展

状态、前提、来源与待确认项见[总览](p05-design-proposal.md)。
本文的名称/字段/操作为语义建议，不是 ABI 或配置已实现声明。

## 1. 基本对象与契约

| 对象 | 定义 | 不等于 |
| --- | --- | --- |
| Module | 有职责、状态 owner 和端口的代码单元 | 线程、进程、动态插件 |
| Capability | 可发现的类型化操作及支持条件，由内置执行器或 App 提供 | 权限，也不保证当前可用 |
| Task | 一次具有身份、期限、状态与反馈的目标执行 | 全部会话、App 实例 |
| App package | 签名内容与 manifest、接口/数据版本、资源要求 | 正在运行的对象 |
| App instance | 包版本、实例代次、权限和资源受控的执行主体 | 全局智脑 |
| Cognitive configuration | 选择认知机制、服务依赖与策略 | 高权限身份 |
| Product profile | 将能力/模块/预算/交互装配为产品 | 新安全层或第二套 runtime |

主机服务和内置能力也有明确 principal，不因为“不是 Wasm”跳过授权。
调用身份由受信边界生成，不能接受模型/App 自报的权限表。

跨模块公共字段只抽取真正共享者：

```text
Identity: device_id, boot_id, principal_id, instance_generation
Time: clock_domain, monotonic_time, observed_at, received_at, valid_until
Source: origin_kind, source_refs, validity, uncertainty, schema_version
Operation: request_id, task_id?, capability_id/version, arguments,
           deadline, cancellation_generation, expected_revision?
Receipt: request_id, state, effect_status, applied_value?,
         observed_evidence?, error_domain/code, retry_class
```

`?` 为按类型可选，不允许无观测时填伪正常值；具体编码、长度与必填规则
在各端口 schema 冻结。回执不携带所有认知上下文；大结果使用有界引用。
标识/序号回绕与 reboot 分域，旧 token 不跨 boot 复活。
普通 C 错误映射保留 `ts_res_t`，增加领域明细而非改旧数值意义。
公共头不包含 OS 类型，也不创建一个囊括所有领域的巨型 common.h。

## 2. 模块清单与唯一所有权

| 模块 | 职责/状态 owner | 输入 → 输出与关键操作 | 依赖/故障边界 |
| --- | --- | --- | --- |
| Assembly/supervision | 装配、启动阶段、系统健康与升级处置；不写业务状态 | profile/服务健康 → 启停计划；prepare/start/stop | 公开生命周期端口；必需保护失败系统 fail-safe |
| Capability catalog | 支持接口、提供者/代次、可用性 revision | 注册/健康 → describe/list/change；register/unregister | 合同；缺可选能力无假成功，冲突注册拒绝 |
| Task service | 请求身份、任务状态、重复与取消账本 | submit/result/cancel → query/receipt | 能力/执行端口；不决定认知目标，容量满拒绝 |
| Dependency service | 网络/模型/存储等事实及变化代次 | backend facts → evaluate/snapshot/change | 后端探测端口；不替 App 直接续跑任务 |
| Resource service | 通道租约、池配额、占用与代次 | reserve/release/revoke → lease/conflict | 合同/时钟；不重写驱动硬件状态 |
| Execution gate | principal 权限、控制来源、期限和资源持有检查 | invoke/cancel → admitted/rejected/result | 上述快照与执行器端口；AI 无修改权 |
| Safety protection | 输出安全态、限幅/变化率/供电与强制故障 | protected output → actual applied/拒绝 | 已有保护机制/设备后端；急停直达 |
| App runtime | 每实例运行状态、执行环境、健康、邮箱与宿主对象 | start/event/stop → lifecycle/progress | WAMR/执行/依赖/资源；宿主故障不能假装 Wasm 已隔离 |
| Delivery manager | 稳定/候选事实与提交恢复 | stage/trial/confirm → durable transition | App/存储/验签端口；详见交付文档 |
| Config service | 用户设置/凭据引用、配置版本 | validated change → snapshot/change | 身份/存储；安全配置不可普通 UI 直接改 |
| Storage service | 命名空间、后端健康、写入/提交结果 | bounded read/write/commit → receipt | 介质后端；不理解人格或改业务对象 |
| Observability | 结构化日志环、丢弃/持久失败、导出状态 | metadata events → status/log/export | 受控存储/外发；失败不阻塞停止 |

这些是可在少量文件中实现的逻辑职责，不要求十二个进程。
任务/App/交付各管不同对象，互相引用 ID，不复制对方状态机。
Dependency 维护事实，Capability 合成自己的可用视图，不复制探测线程；
Resource 的 lease 不替代既有网络控制租约，执行门同时检查两者。
Memory 自己管理经历和索引；Storage 只管理介质和提交，不成为智脑。

### 生命周期与启动

推荐模块语义：`unprepared → ready → active → stopping → ready`，
准备或清理失败进入 `faulted`；禁用模块无实例，不伪注册可用能力。
不是所有模块必须支持运行时停用，保护与基本配置/修复入口持续有效。

现有安全启动次序是受保护基线。本轮不把新依赖 DAG 当成改顺序许可。
实施时先在既有启动后装配服务，若必须调整顺序，单独确认并回归：
保护先于普通输出、必需状态先于消费、失败不能半激活。
网络初始化成功与云端可用分开；云不可用阻止智脑激活，不要求整机启动失败。
必需记忆健康在相应配置激活前检查；缺卡不创建空白实例。
服务退出逆向释放其持有资源，但保护停止不能等完整逆序关闭。

## 3. 能力调用、任务与授权

Capability descriptor 推荐包含稳定 ID、接口版本、typed argument/result、
提供者与代次、权限要求、依赖、资源、最大等待、幂等性和完成观测方式。
实现简单能力只需明确函数；跨 App/模型工具边界才序列化，拒绝无 schema
的任意 JSON 工具调用。目录查询区分支持、当前可用、调用者获准。

任务建议：

```text
created → admitted → running → completed | failed | unknown
created/admitted/running → cancel_requested → cancelled | unknown
created → rejected
```

等待依赖可作为声明的 admitted 子状态；超时原因进入回执，不伪 completed。
已完成结果先于 cancel 时仍是 completed，取消返回“已结束”；不要改写已发生事实。
取消接受后禁新副作用，已提交效果可有最后回执；无停止/完成观测时保持 unknown。
`effect_status` 区分未提交、已提交未确认、观察到效果、无观测，任务完成由操作
合同决定。例如诊断完成不需要物理动作，物理执行则不能用影子值证明到位。

重复键推荐 `(principal, request_id, boot/session domain)` 加内容摘要。
同键同内容返回已有状态，不再次执行；同键不同内容拒绝。
账本容量和留存期声明，过期后不再宣称重复被消除。
跨重启只对需要恢复且已持久化的任务提供重复保护；未知非幂等操作不重试。
可恢复日程存 deadline 类型/墙钟有效性，不保存 uptime 后跨 boot 直接相减。

执行链：

1. 受信入口绑定 principal、请求/实例代次，验证 schema 与版本。
2. 校验权限、来源/绑定期限及当前任务状态。
3. 原子申请需要的资源集合；首期固定全序申请，冲突拒绝/有界等待，
   不边占边无限等待，不自动抢占其他 App。
4. 在副作用提交边界复查取消/授权/资源 generation、期限和依赖。
5. 受保护输出经原安全提交，其他能力走各自受控执行器。
6. 执行器反馈提交与观测，任务服务更新并释放；失败释放未提交预留。

描述快照用于解释，不替代第 4 步实时复查。取消、撤销与最终提交必须共享
同步/代次规则：若取消先线性化，后续提交被拒；提交先发生则如实报告效果。
停止令牌/资源租约也在异步队列消费时复查，不能入队检查一次后永久授权。
跨多个输出不承诺原子物理操作；有部分效果时逐项报告，不伪回滚。

GPIO/PWM/供电仍走现有 `ts_safety_commit*`。音频/显示等不能硬塞进
现有输出 union；使用同样不可绕过的授权/停止合同及各自后端约束，
具体保护实现须另验证，不声明当前 safety 已覆盖全部媒体输出。

## 4. 依赖失效、取消与恢复

依赖事实建议状态：`unknown / unavailable / checking / available`，
携带原因、验证时间、有效期、revision。区分网络关联、DNS/TLS、
服务身份、特定模型、凭据/额度，不以心跳替代所有服务探测。
探测及单请求重试各有预算；防抖控制整体激活，不拖延受影响任务停止。

每个 App/智脑配置声明必要依赖和失效动作：未启动拒绝/等待；
运行中停止/失败或有验证语义的暂停；首期推荐停止重建，少承诺复杂续跑。
已确认的本地独立 App 按自身依赖继续，不附加“必须智脑在线”的隐式条件。

失效顺序：先提高任务/会话失效代次、关新提交 → 通知 owner 取消在途
模型和输出 → 清理资源/待播 → 写有界诊断；物理停止优先，不等日志。
请求取消云端操作不保证云端实际停止计费，设备必须独立拒迟到结果。
若后端停止不确认，隔离资源而非提前复用 DMA 缓冲或启动另一输出。

恢复：验证全依赖 → 核对版本/实例/任务/用户控制 → 激活允许对象 →
后台分析已授权日志。取消/静音/关闭自主交互持续有效，不自动续旧动作。
暂停任务仅在其明确续跑合同允许时恢复；不作“所有任务恢复”默认。
云正常但产品所需记忆损坏时智脑仍不激活；基本修复入口本地可用。

## 5. 线程、资源与故障域

MCU 推荐延续已验证载体再减少耦合，不先写新调度器：

| 执行域 | 承担 | 禁止/成本约束 |
| --- | --- | --- |
| ISR/独立保护路径 | 硬件停止、最小信号与状态失效 | 无模型/Flash/动态分配；不等待普通事件队列 |
| 有界控制推进 | 任务/依赖/资源状态迁移，短回调 | 不同步做网络、整库扫描或长 codec |
| 网络 worker | 请求 IO/分片，结果送回 owner | 超时/取消及缓冲有界，不能持执行锁等待 |
| App 执行域 | 当前每实例线程/邮箱，串行回调 | 解释器计量不封住宿主阻塞；宿主调用另定上限 |
| 认知推进 | 状态快照、触发、候选校验 | 等模型期间让出；不占媒体实时线程 |
| 存储/维护 worker | 经历写入、巩固、日志与事务 IO | 优先级/写入预算，不能持保护锁擦 Flash |
| 媒体后端 | DMA、音频块、帧交接 | 按后端需要线程，不每处理节点建线程 |

是否复用 worker 由阻塞性质和目标板证据决定，不给未测线程数。
同步短事件回调只做快照/投递；禁止回调内反向等待发布方。
旧事件借用指针必须维持调用域，异步改造改为有界复制/owner 引用并逐消费者迁移。
建议锁规则： owner 自身锁不跨模块调用；资源申请统一全序；
执行提交临界区不等外部 IO；取消失效位可从停止路径设置。

配额计入 Wasm 线性内存、模块/宿主栈、邮箱、网络/TLS、媒体池和存储缓存，
不是只检查 `heap_kb`。区分内部 RAM/PSRAM/DMA 可达与最大连续块。
队列策略按类型：控制请求满明确拒绝；状态变化合并最新并标丢计数；
媒体按会话约定阻塞/丢弃/终止；取消走独立有界路径，不靠挤掉普通消息。
依赖状态失效不可仅依靠可能丢失的通知，提交时读取有效 revision。

单 App 失败先撤调用权、收资源并有界停止。当前 WAMR join 超时不可当
正常卸载；保留未退出对象，隔离共享输出，按平台合同受控重启/报告故障。
Linux 可增加独立进程隔离，但不是首期默认隔离承诺；
MCU 原生驱动/共享 WAMR 宿主错误可能升级系统故障。
看门狗区分各执行域活性，不能用“任一 App 活着”证明全部 App 健康。
实现加强前保留现有测试与群组行为，变更单独验证。

## 6. 可选媒体与数据面

| 模块 | 状态/接口建议 | 关键约束 |
| --- | --- | --- |
| Image | session/request owner；describe/capture/result/cancel/release | 新帧/采集时间；内部可 stream；不是实时视频合同 |
| Audio input | capture session；start/read block/stop | sample format/rate/channels、序号/时钟域、丢块可见 |
| Audio output | playback session；start/write/progress/stop/drain | 接收与已播分开；取消丢待播，drain 正常排空 |
| Display/expression | 输出 owner；有限 render/update/clear | 场景/动画属于产品，不在核心内置 UI |
| Processing adapters | 有界 AEC/codec/重采样等可选链 | 支持格式明确，未验证不静默转换 |

每个媒体 buffer ref 带池/句柄代次、范围、类型和所有者；禁止伪造、
重复释放及过期访问。CPU/设备共享时由后端处理 cache/DMA 同步。
首期推荐简单 session ownership 和少量固定池；引用计数只在确有多个消费者时用。
Wasm 请求控制与有界复制，不读宿主指针；宿主可优化传递但需生命周期证明。
取消收完 driver 仍持有的缓冲才复用；超时不能通过强行 free 制造“停止成功”。

采集、保存、外发、播放权限分开。外发同时检查数据类别、目标服务、
授权范围/代次和会话有效性；不把既有 `av:read` 自动扩成任意上传权。
旧 AV 接口迁移要显式版本/授权兼容处理，不能静默改变旧客户端行为。
语音识别/TTS/视觉推理通过模型/服务适配，捕获权限不自动授予这些服务。
单帧、AEC、真人打断属于产品验收项；后端可取消不证明声学体验通过。

## 7. 运行时定向验收

- 同一合同向量测能力缺失/版本不匹配/无权限/不可用四种不同结果。
- 注入取消与提交的两种顺序、重复/乱序、代次撤销与重启；
  断言无新迟到副作用，并如实保留先发生效果。
- 依赖抖动/凭据失效/内存不足并发，验证必要对象停而本地独立 App 继续。
- worker 卡死/宿主阻塞/队满，实际停止优先且资源未被错误复用。
- 关闭媒体/情感/经历配置运行辅助三个场景，不仅编译通过。
- 跨后端回执/schema 一致；时序/资源/隔离按支持矩阵实测，不强求平台逐位等同。
