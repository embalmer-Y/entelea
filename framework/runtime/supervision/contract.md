# R02 监督模块合同（events / wdt / boot）

迁自旧 core/{events,wdt,boot}.c；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## 事件总线（events）

- 静态注册表，分发顺序 = 注册表静态顺序，禁运行期插队；每事件容量
  `ENTELEA_EVT_MAX_SUBS`(4，DEC-27)，满 = `ENTELEA_E_NOMEM`；仅 thread 注册。
- thread 发布 = 同步分发；ISR 发布 = 投递深度 `ENTELEA_EVT_QUEUE_DEPTH`(16)
  的队列由 sysworkq 分发，满 = 丢弃并计数（观测损失，非安全损失）。
- **ISR 投递的 `data` 必须指向静态存储（借用寿命）**：分发延迟到 drain，
  栈载荷未定义。此约束无机械检查，靠评审与用例示范承担。
- 时间戳由总线用绑定实例时钟的原始 read 加盖（发布瞬间）；未绑定或
  从未成功读到时 `t_ms=0`（可观测的未配置，不伪造时基）；读失败/回退
  沿用最后有效值（单调护栏）。原始 read 绕过 `clock_now` 的非原子回退
  簿记；护栏簿记（u64 RMW）在 irq_lock 微临界区内比较/更新——ISR 发布
  与线程分发并发访问、32 位目标不可撕裂。

## 软看门狗（wdt）

- 源静态注册（started 后拒绝）；**激活 = 首次 feed**，未激活源不判逾期；
  `deactivate` 为停止后预期态；`grace` 豁免窗绝对截止、60s 钳制、feed
  清除、不激活未激活源（语义同旧 DEC-48①/DEC-64①）。
- 巡检周期 = min(periods)/2，无注册源按 `ENTELEA_WDT_HARD_CAP_MS`(10s)。
  逾期 → `ENTELEA_EVT_WDT_WARN`（带 src/last_feed）→ `system_fail`
  （FAULT reason 编码 `ENTELEA_FAIL_WDT`）；巡检同时驱动 estop 补发。
- `CONFIG_TASK_WDT` 时挂 L3 通道（5000ms，callback=NULL → 过期即
  sys_reboot）；`ENTELEA_WDT_REQUIRE_HW` 编译定义 + 缺 chosen watchdog →
  `ENTELEA_E_IO` 显式失败，不静默软兜底。
- 时钟实例绑定；未绑定 register/start = `ENTELEA_E_UNINIT`。
  feed [any]：绑定时钟 read 须 ISR 安全（平台 uptime 满足）；时钟簿记
  （护栏/失败计数）在 irq_lock 内维护（ISR/线程并发）。
- **时钟失明策略（A1，owner 2026-11-08 定案）**：单轮巡检时间不可知
  （读失败/回退由护栏接管）→ 本轮不判逾期、不假活；连续
  `ENTELEA_WDT_BLIND_ROUNDS`(3) 轮不可知 → 监督无法计时 →
  `system_fail(ENTELEA_FAIL_WDT_CLOCK_BLIND)`（载荷 0xFF 非源编号），
  好读复位轮数。监督在时间不可用时不冒充活性。
- `entelea_wdt_halt()` 一次性全局停机（`system_fail` 先行调用）：置位后
  巡检不喂、不重排、不判逾，`wdt_start` 拒绝；无解除路径（生产语义），
  ENTELEA_TEST 复位钩子除外。`entelea_wdt_feed` 与巡检的 task_wdt 喂狗
  在**入口**自守卫 halt（三轮 review 发现 6）——粒度为调用入口，至多
  放行一次已在飞的调用。
- **时钟采样串行化（三轮 review 发现 3）**：三模块的时钟簿记把
  读+比较+更新全程放在 irq_lock 微临界区内（读取须短且不阻塞——平台
  合同约束）：线程/ISR 采样完全串行，正常前进不再被交错误记为回退；
  真回退（时钟自身后退）计故障并保守沿用最后有效值，仍按 A1 计入失明
  轮数（监督读者反复拿到过期值 = 计时不可靠）。
- R02 源集合仅 `SYWORK`；NET/APPMGR 源随其子系统批次追加编号。
- 相对旧实现裁剪：noinit 留痕与上次复位读出归 R03。

## 监督启动（boot）

- 执行器消费平台装配表（platform/zephyr/boot_steps.c）；顺序是规格：
  estop_gpio → safety_poweron → wdt_start → supervision_ready（旧
  core_init 占位更名），只允许尾部追加。每步完成发布 `BOOT_STEP`
  （含失败步 result）；任一步失败 → `system_fail(FAIL_BOOT(i))` 后受控
  停机（停喂 WDT → 兜底复位），不得半启动。全部成功发布 `BOOT_DONE`
  后返回，main 转常驻监督空闲。
- 装配（main）在执行器前完成三处时钟绑定；绑定失败按 `FAIL_BOOT(0)`
  fail-safe。业务步骤（net/app/audit）由后续批次尾部追加，不进本表。

## 测试钩子

`ENTELEA_TEST` 编译定义暴露 `poll_drain`/`patrol_once`/`grace_active`/
`test_fail_reason`/`test_reset`；生产构建不可达。
