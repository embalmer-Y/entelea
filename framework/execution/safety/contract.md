# R02 输出保护合同（channel / commit / force / drivers）

迁自旧 safety/{channel,commit,force,driver_dispatch}.c；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## 通道与三安全态（channel）

- 通道 desc 注册期冻结（uid/kind/三安全态/limits 运行时只读）；
  三态缺一/值越限/uid 非法或撞名 = `ENTELEA_E_PARAM`（不留半注册）；
  容量 32（DEC-27）满 = `ENTELEA_E_NOMEM`。
- 状态机：SAFE_POWERON →（link up）→ ACTIVE →（断链）→ SAFE_LINKLOSS
  →（link up）→ ACTIVE；fault 从任意态直达。恢复/重附**不自动回写**
  旧值——须显式 commit（输出连续性由上层承载）。
- `poweron_init` 把声明 poweron 值落驱动（boot 步骤 2；forced 通道跳过
  保持 SAFE_FAULT；写失败聚合 `E_IO` → boot fail-safe）；`set_link`
  （返回 `E_IO` 当任一迁移写失败）/单通道迁移与 commit 经 write_lock 互斥，
  写+影子+状态全在 irq_lock 内完成（estop ISR 抢入不丢失 fault 语义），
  事件在解锁后且复查未 forced 时发布。
- `force_channel_fault`（detach 机制面）不置全局锁存；`channel_recover`
  = 重新上电语义，全局锁存期拒绝。`clear_fault` 是 estop 锁存唯一释放
  路径：条件恢复 + 输出值不回写（保持 fault 安全值直至显式 commit）。
  运行期可达性（sys:estop-clear 命令面）归后续批次；R02 直调仅供测试。

## 唯一写路径（commit）

- 一切产品输出必经 `entelea_safety_commit`：查表 → ACTIVE 校验（安全态
  /锁存期 = `E_STATE`）→ 限幅（PWM clamp = `E_RANGE`）→ **slew 时钟门**
  （本次时钟读失败=时间不可知 → 速率界不可立 → 整笔 `E_STATE`；持续
  失败期间重复提交不得逐步改变输出）→ slew（超率拆分到界值 =
  `E_RANGE`，实际落值经 readback 可观）→ 限流（POWER 超限整笔拒 =
  `E_RANGE`）→ 末段临界区（irq_lock 复查 forced；后端写失败 = `E_IO`，
  影子/基线/golden 记录不更新，不报告从未落驱动的“成功”）。
- [thread] API：ISR 调用 = `E_PERM`。锁序 write_lock → 事件发布，单向。
- `commit_locked` 供已持锁方（R04 预算检查-提交原子化）；R02 无外部持有者。
- **失败后的执行资格（A2，owner 2026-11-08 定案）**：安全态写失败
  （断链/detach/recover 的落驱写）= 物理效果未知 → 通道进 `SAFE_FAULT`
  （撤销输出资格：其后的 commit 一律 `E_STATE`，即使后端已恢复——须显式
  `channel_recover`）；影子保留最后确认值（不伪称安全值已落驱）。
  commit 自身的后端失败则保持 ACTIVE（瞬态可重试；外部效果未知的
  非幂等动作是否允许自动重试属调用方合同，本模块不替其决定）。
- **释放资格按故障来源（三轮 review 发现 1）**：`clear_fault` 仅释放
  estop 锁存面；安全写失败所致 SAFE_FAULT 的通道被跳过（仅
  `channel_recover`），后续**成功的** fault 落驱（确认写）会覆盖早期
  “物理未知”标记并回到可释放面；`system_fail` 为终态（对齐 wdt halt
  一次性），其后 `clear_fault` 返回 `E_STATE`。
- **estop/system_fail 落驱失败与升级（三轮 review 发现 2；升级策略
  owner 2026-11-08 定案①）**：失败通道影子保留最后确认值（不伪称
  fault 已落驱）、状态 SAFE_FAULT/recover-only、计数留痕；强停序列
  先尽力安全化其余通道，**任一落驱失败 → 立即 `entelea_wdt_halt`**
  （原子置位，estop ISR 上下文安全；停喂后由 WDT 复位收口——“无法
  保证输出达安全值即停机”）。范围：estop/system_fail 强停面；单通道
  detach 失败维持 A2 按通道语义，不升级整系统。
- **slew 总预算（三轮 review 发现 4；基线语义 owner 2026-11-08 定案 C）**：
  界 = slew × (now − last_t)，锚定最后一次**确认写**（值+时刻）；无每笔
  +1 免跳——同一毫秒内任意次提交的总变化 ≤ slew。**任何确认写**
  （commit/poweron_init/断链/fault/recover 落驱成功）都刷新锚点；激活、
  clear_fault、写失败不重置基线（无物理确认即无新锚点）——输出永远从
  最后确认值受控爬升，estop→clear→commit 循环不再产生免跳。注册后
  从未确认写过的通道首笔仅受 min/max/限幅（无锚点场景；真实启动流中
  poweron_init 已建基线）。溢出饱和到 u32 极限（确定性）。时钟门覆盖
  基线建立与后续提交。
- **事件语义（三轮 review 发现 7）**：`SAFE_STATE_CHANGED` 是**历史迁移
  事实**（“某时刻迁移到 X”），非当前状态通知——estop 可在发布前复查与
  分发之间抢入；消费者不得据事件推断当前态，权威态以
  `channel_state`/`summary` 查询为准。不为制造表面原子性而在长关中断
  区内同步执行回调。
- 时钟簿记（单调护栏/失败计数）在 irq_lock 微临界区维护（ISR/线程并发，
  32 位目标 u64 防撕裂）；`entelea_safety_clock_faults()` 可观。
- **相对旧实现裁剪（延后批次，非行为声明）**：审计环形与 flash 下沉
  （R03）；控制源仲裁 `set_arb`/`commit_src`/压制门控（R04）。R02 拒绝
  事实经返回码与状态可观。

## estop / fail-safe（force）

- `force_all_fault` [ISR] 直达：仅原子置位 + 驱动直写，调用图内禁分配、
  队列、锁、协议栈；`ENTELEA_EVT_ESTOP` 由 sysworkq 周期补发（ISR 内
  不提交工作项），响应上界 = 巡检周期 + 调度延迟。
- `system_fail` [thread]：全通道 SAFE_FAULT + 受控停机（停喂 WDT →
  兜底复位）；`ENTELEA_TEST` 构建置态后返回供断言。noinit 留痕归 R03
  （R02 仅内存 reason 经测试钩子可观）。

## 驱动分发（drivers）——全库唯一 Zephyr 输出驱动调用点

- `source-policy.json` 的 `protected_output_sources` 保护门 = 本目录
  drivers.c；架构机械检查禁止其余一切源直接调用 gpio/pwm 写 API。
- 写函数返回 0/负 errno，只依赖注册期冻结数据（可重入/无锁）；**成功
  才**记录写序列（每通道 32 笔环形=已执行写，golden/对拍数据源）。
  绑定通道（uid 匹配 DT 绑定）后端未就绪（未 init/失败）= `-ENODEV`
  显式失败，不得静默退化为软件成功；**未绑定通道 = 纯软件通道（A3，
  owner 定案）**：写成功仅指软件事实（native_sim/CI 语义），板级是否
  强制全绑定留 D12/板级批次。
- 真机后端为板级替换面：GPIO（zephyr,user 节点 uid+io-gpios）、PWM
  （pwm-uid+pwms；u 低16=permille、高16=hz/100），`ENTELEA_DRV_GPIO`/
  `ENTELEA_DRV_PWM` 编译定义启用；缺 DT 绑定 init = -ENODEV。PWM 后端
  失败进 `pwm_err` 计数（void 写函数不报告成功）。native_sim/CI 为桩。
- estop DT 绑定走 aliases `ts-estop-gpio`；无节点板（native_sim/测试）
  init 空操作成功，生产板必须提供（板级断言随参考板冻结）。触发边沿
  R02 固定缺省上升（prov 化归 R03）；通用 GPIO 输入后端未迁入。
