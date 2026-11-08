# R06 网络机制/动态依赖合同

迁自旧 net/linkmon.c + lease.c + cmd.c 幂等缓存语义 + 新-only 依赖
状态机；方向与未决项以[执行规划](../../../docs/rebuild-plan.md)第 7
节为准。**假传输注入**：本批不碰真实网络/云——依赖状态机消费调用方
上报的观测（DNS/TLS/云失败 = 假传输上报 bad；真机批替换观测源）。

## 心跳监视（断链判定源）

- host 心跳到达 streak++；**恢复滞回**（连续 ≥2 次才 up——DEC-27）；
  超时（elapsed > 6×1000ms——DEC-22）→ down（立即，失败侧保守）。
- 部署模式守卫（CG2）：仅 orchestrated 把判定喂给
  `entelea_safety_set_link`；standalone 仅观测（本地逻辑照常）。
- **"TCP 连着"≠链路健康**：判定只看心跳序列，不看 socket 状态。

## 控制租约（准入仲裁）

- V1 单租约；TTL 10s（DEC-41）；**惰性过期**（访问时判定，无定时器）；
  lease_id 单调递增不复用。同 holder 重取 = 幂等续期（id 不变，窗口
  顺延）；他人持有 = 拒 + 回填当前租约归因。
- **本地席位联动**（CG3）：arb=LOCAL 期间 acquire 拒、held_by 恒
  false——**视图语义**（不改在册状态；TTL 照常走完，释放后旧租约若
  未过期仍有效）。只管命令准入，不联动输出安全态（安全态唯一判定源
  = 心跳监视/estop）。

## 幂等缓存

- idem 命中 → 回放缓存回执**不重执行**（replayed=true）；定容 LRU
  （4 槽，DEC-40），回执 ≤128B（本批预算）。
- **unknown 非幂等不自动重试**：exec 返回非 OK（含 PENDING/unknown
  效果）**不缓存**——下次同 idem 重执行；重试决策归调用方（本层不
  静默重试、不把 unknown 冒充已确认）。

## 依赖状态机（N-DEPENDENCY 新面）

- 四类依赖：network/model/storage/app（声明→可观测态）。
- 恢复防抖：DOWN→UP 需**连续 2 次** good 观测（中间态 DEGRADED——
  抖动可见）；UP→DOWN 立即（保守）。
- 独立 App 不被全局智脑状态绑死：App 判**自己的**依赖子集
  （`entelea_dep_state(APP)` 等），不做全局状态闸。

## 未测与边界（如实）

真实 DNS/TLS/云探测（观测源替换归真机批）；真会话重连（zenoh 归
后续真机批——本批机制不含传输）；风暴实测阈值（D08 数值待真机校准）；
消息路由/发布队列（avq/pubq **传输机制**归本批后续/R09 联合；媒体
外发决策/内容面归 R09 闸——计划 L207/L373；两侧均未实现，归属经
计划符合性审查一致化）。
