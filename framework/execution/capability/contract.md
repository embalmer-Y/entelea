# R04 能力/裁决合同（principal / 能力注册 / 资源 / 任务）

新实现（N-CAPABILITY/N-RESOURCE/N-TASK 新-only；perm/desc 迁自旧
hal/periph 语义面）；方向与未决项以[执行规划](../../../docs/rebuild-plan.md)
第 7 节为准。

## principal 与能力文法

- principal 在 `context.h` 前向声明，完整结构公开于 `principal.h`。
  **可信宿主**必须经 bind 构造并控制传递（槽上限 4、app_id 撞名拒绝）；
  解绑后旧句柄裁决 `E_PERM`。槽号、代数和 app_id 均不是秘密：
  同地址空间原生代码若自行构造完全匹配的字段，会被当作同一主体，
  因此本机制不提供 C 层不可伪造性或原生进程隔离。Wasm native 必须
  由宿主注入绑定主体，不能将不可信参数解释为主体。
  句柄 = 槽号+槽绑定代数+app_id，三者须与槽当前绑定一致：同槽重绑
  （他 app 或同 app）不复活旧副本，过期副本解绑为空操作（RF1 P1-1）。
  每槽代数为 uint16，达到 `UINT16_MAX` 后该槽本次运行不再复用；
  所有槽忙或耗尽均返回 `E_NOMEM`，不回绕。句柄不能持久化或跨重启复用。
  绑定、解绑和裁决使用同一短自旋锁，锁内不发布事件；裁决只保证其
  自身时刻的授权，成功后动作仍需调用方协调撤销及在飞操作。
  临界区的真实硬件中断延迟尚未测量。
- 文法 `class:op:instances`（数值位图 0-31；"0-3"/"0,2"）与 msg 名单
  `msg:send:name`（[A-Za-z0-9._-]{1,39}，名单≤4）；域外类/操作/实例
  >31/文法坏 = `E_PARAM`（fail-closed，不静默截断）；重复声明幂等。
- 越权一律拒绝并尝试发布 `EVT_PERM_DENIED`（app_id+detail 观测面）。
  ISR 路径复制载荷，队列满时事件可能丢弃并计入总线丢弃数。
- msg 仅 send 有门（recv 被动无权限门——旧语义保持）。
  裁决仅比较 1–39 字节、符合文法且有结束符的名称；无结束符的表项、
  超长或非法输入不授权并留痕（`E_PERM`），`NULL` 为 `E_PARAM`。

## 能力注册（描述符）

- 静态批量注册（boot 期）：**原子**——任一非法/已注册撞名/批内撞名/
  超限 → 整批拒绝零注册（回滚不留半注册）。输出类必带 safety uid。
- `acquire` = 权限门（输出=WRITE、输入=READ）+ 输出通道须 ACTIVE；
  SAFE_POWERON/detach 后 = `E_STATE`。**直接 safety commit 绕过本门
  属调用方违约**（评审面；机械强制待 R05 natives 统一面）。
- `detach` = safety 单通道 fault（机制面复用；A2 资格语义适用）。

## 资源（功率预算）

- 预算源 = prov（运行时只读，合同 10；未配置=0=一切供电请求拒绝——
  缺省安全侧）。记账单源 = Σ safety readback（不设影子账本）。
- `power_request`：SET 门（power:write）→ 预算检查（含**本通道替换
  先扣**语义）→ `write_lock` 内 `commit_locked`（检查-提交原子化；
  锁序复用无新锁）。超预算整笔拒绝（无在线降额）；关闭不受预算限。
- 峰值观测顺带更新；快照面 slots=POWER 通道数。

## 任务状态机

- 有界表（4）；受理去重（同 principal+同 seq 非终态 = `E_BUSY`，
  不静默合并；重启重试用新 seq——幂等键归上层）；submit 仅成功时写
  out（失败不清零调用方 id）。
- 状态：ACCEPTED→RUNNING→SUCCEEDED/FAILED；任一非终态→CANCELLED。
- 效果域（复用 R01 `entelea_effect_t`）：ACCEPTED 期取消=NONE；
  RUNNING 期取消/失败=UNKNOWN（已发生效果不撤回、不可确认）；
  成功的 CONFIRMED 由调用方据真实回执给出——**本层不伪造确认**。
- 他者任务操作=E_PERM；重复 start/终态后再操作=E_STATE；终态可
  回收复用。**unknown 非幂等动作禁止自动重试**（效果域置位如实呈现，
  重试决策归上层合同）。
- 锁：单 mutex（受理/迁移/终态/回收串行）；get 无锁快照（观测容忍
  微撕裂，权威态同查询面）。无模型：受限测试 principal 直调闭环。
- **截止时间**（计划步骤条目，owner 2026-11-09 A1 决议补交付）：
  submit 携带 `deadline_ms`（实例时钟绝对毫秒；0=无截止——不依赖
  时钟）。受理与开始门：带截止而时钟未绑/读失败/回退=`E_STATE`
  （时间不可知不受理、不开始）；受理即过期=`E_RANGE`；已受理的
  非终态截止任务存在时，切换不同 clock_id=`E_BUSY`。时钟读取、
  单调簿记与状态迁移由同一 mutex 保护，取消仍可在时间不可知时
  使用。惰性过期（无定时器——确定性
  纪律同 R06）：线程入口（submit 去重扫描/start/终态/cancel/
  release）在锁内检查，ACCEPTED 期过期自动转 CANCELLED（效果
  NONE——未开始无副作用）并即时让出受理去重位；RUNNING 期过期
  **不**自动迁移（效果未知不代决——调用方经 task_get 观察
  deadline 后显式 cancel）。task_get（[any] 观测）不触发迁移。

## 未测与边界

并发压力/多核（native_sim 单核语义）；hal natives 统一面与 fs/av/log
等类消费者归 R05+；调度公平性合同（规划闸"锁序/公平"）在多任务真实
并发接入时评审；仲裁面见 safety 合同（R04 节）。

**已接受的边界（owner 2026-11-23 确认，RF1 WP5）**：首期把同地址空间的
原生代码视为可信。句柄不提供 C 层不可伪造性，本框架首期不承载不可信的
原生扩展；将来若需要，须另定隔离与句柄合同，不能依赖字段保密。把 Wasm
natives 统一经执行门的机械强制列为有界加固项，尚未实施。
