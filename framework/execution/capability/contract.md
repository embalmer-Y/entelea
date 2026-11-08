# R04 能力/裁决合同（principal / 能力注册 / 资源 / 任务）

新实现（N-CAPABILITY/N-RESOURCE/N-TASK 新-only；perm/desc 迁自旧
hal/periph 语义面）；方向与未决项以[执行规划](../../../docs/rebuild-plan.md)
第 7 节为准。

## principal 与能力文法

- principal 完整定义源=R01 context.h 的 `struct entelea_principal`
  （前向声明同源）；**bind 是唯一构造入口**（宿主可信面，槽上限 4、
  app_id 撞名拒绝）；解绑后旧句柄一切裁决 `E_PERM`（伪造/过期不可绕）。
- 文法 `class:op:instances`（数值位图 0-31；"0-3"/"0,2"）与 msg 名单
  `msg:send:name`（[A-Za-z0-9._-]{1,39}，名单≤4）；域外类/操作/实例
  >31/文法坏 = `E_PARAM`（fail-closed，不静默截断）；重复声明幂等。
- 越权一律拒绝并发布 `EVT_PERM_DENIED`（app_id+detail 观测面）。
- msg 仅 send 有门（recv 被动无权限门——旧语义保持）。

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

## 未测与边界

并发压力/多核（native_sim 单核语义）；hal natives 统一面与 fs/av/log
等类消费者归 R05+；调度公平性合同（规划闸"锁序/公平"）在多任务真实
并发接入时评审；仲裁面见 safety 合同（R04 节）。
