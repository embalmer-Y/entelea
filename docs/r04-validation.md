# R04 能力/权限/任务/资源验证

授权边界：R04 按 owner 目标指令自主推进（软件机制依赖=保护端口已就绪；
台账批次状态机因 R02 硬件闸 owner-blocked 而如实保持 pending——软件面
交付经本记录与 N-* 资产承载，见批次 note）。未决阈值按建议值实时实施
并记录（见"自行决策"）。

## 交付范围（提交 `4bc4a1d`）

- principal/裁决（framework/execution/capability）：完整定义源对齐 R01
  context.h（`struct entelea_principal` 单一定义）；bind 唯一构造入口；
  文法+msg 名单 fail-closed；`EVT_PERM_DENIED` 留痕。
- 能力注册：原子批量（任一非法/撞名/超限 → 整批回滚零注册）；acquire
  =权限门（输出 WRITE/输入 READ）+ 输出通道 ACTIVE；detach 复用 safety
  单通道 fault（A2 资格语义适用）。
- 资源（framework/execution/resource）：prov 只读预算（0=全拒）、
  readback 单源记账（Σ 已启用通道，无影子账本）、**本通道替换先扣**、
  write_lock 内 `commit_locked` 检查-提交原子化（锁序复用无新锁）。
- 任务（framework/execution/task）：有界表（4）、受理去重（同
  principal+seq 非终态=E_BUSY；submit 失败不清零 out）、取消效果域
  （ACCEPTED=NONE/RUNNING=UNKNOWN——效果不撤回不可确认）、CONFIRMED
  由调用方真实回执给出（不伪造）、他者拒、终态回收。
- safety 仲裁面（R02 遗留落点）：set_arb（原子位+事件，零通道写入，
  幂等）/commit_src（LOCAL 席位激活期压制 REMOTE，先于通道态，异常
  审计留痕；APP/LOCAL 不受门控）。

## 验收矩阵与真实结果（2026-11-08；r04 套件 5/5，native_sim 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 伪造/过期句柄不可绕 | 域外(9)/零/解绑后句柄全 E_PERM | test_principal_grammar_and_forgery |
| 文法 fail-closed | 域外类/操作、实例>32、逆区间 3-1、msg 非 send、名单外字符全 E_PARAM；重复声明幂等 | 同上 |
| 半注册回滚 | 批内撞名/输出无 safety uid → 零注册（find=NULL 证实） | test_capability_registry_and_gates |
| acquire 门 | SAFE_POWERON 期 E_STATE；detach 后 E_STATE；未声明 principal E_PERM | 同上 |
| 受理去重/幂等 | 同 principal+seq 非终态 E_BUSY；重复 start E_STATE | test_task_lifecycle_and_race_edges |
| 取消效果域 | RUNNING 取消=UNKNOWN（断言 effect 域）；ACCEPTED 取消=NONE；终态后取消 E_STATE | 同上 |
| 所有权 | 他者 cancel/succeed/release 全 E_PERM | 同上 |
| 槽位有界 | 回收 2 终态后 4 新任务恰好占满、第 5 个 E_NOMEM | 同上 |
| 预算原子/边界 | 450+400>500 整笔拒且既有 450 不变；替换先扣边界 500 通过、+1 拒；关闭不受限；预算 0 全拒 | test_power_budget_atomic_request |
| SET 门 | 无 power:write 声明 E_PERM（先于预算检查） | 同上 |
| 仲裁压制/幂等 | LOCAL 激活期 REMOTE=E_STATE、APP/LOCAL 放行、同席位幂等零事件、释放后恢复、域外席位 E_PARAM | test_arbiter_suppression |
| 回归 | safety/store 套件 SUCCESSFUL；主机 97 项+CTest 6/6+架构 34 源恰分类 | 本轮实跑 |

## 自行决策（建议值，待追认）

① 任务表上限 4（借用旧 DEC-27 #15 语义面——正式值随多任务真实接入
评审）；② 受理幂等键=（principal, seq），重启重试=新 seq（幂等缓存归
上层）；③ acquire 输出=WRITE 门/输入=READ 门；④ 预算替换先扣（同通道
重请求按差值计）；⑤ 仲裁缺省 REMOTE（旧现行为零变化）。

## 未测与边界（如实）

并发压力/多核（native_sim 单核语义）；natives 统一面与 fs/av/log/kv
类消费者归 R05+；UNKNOWN 效果"禁止自动重试"是呈现层语义（调用方合同
面）；无锁 get 快照容忍微撕裂（权威态=同查询面）；公平性合同待多任务
真实并发评审。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/r04 \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r04-texec/zephyr/zephyr.exe
```

## 计划符合性审查补记（2026-11-08，轨道 1）

- 出处登记补正：periph/desc.c→capability/desc.c、power/budget.c 与
  power/slots.c→resource/power.c 已在台账登记 migrate（commit 4bc4a1d）
  ——原代码注释声称"迁自旧"而源项 pending_review 的不一致消除。
- 如实点名（处置归计划修订/轨道 2）：计划步骤"截止时间"条目零落点
  （类型已备于 app.h/time.h）；A-LEASE 输入唯一源 lease.c 处置指向
  R06（net/netmon.c），本批未显式记录租约面去向。
- review P1 修复分支（预算锁内原子性/效果域锁内写）复核在位。
