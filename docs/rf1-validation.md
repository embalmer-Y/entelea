# RF1 迁移复核与整改记录

## 结论与证据边界

独立复核的原始基线是 `84fb440`；RF1 已提交 G1–G4（截至
`f5160ae`），后续 G5/G6、追加反例修复及本文在其上继续修改。
本文所在提交及后续台账记录可用 Git 历史确认；本地构建目录不是源码
版本锚点。RF1 **仍 blocked**：WP2 已把试用确认的权限列表与运行时 natives
授权取交集，并由设备计算候选摘要（F1/F3/F4）；WP3 已实施稳定集准入（D-E）；
WP4a 已实施 D12 日额持久化与墙钟、互斥、持久端口（板级墙钟待 SNTP 接线）。
工厂权限映射与板级试用持久化闸未闭合，不能宣布软件安全闭环或框架/产品验收完成。
R13 保持 pending。

本轮未烧录、操作硬件、调用真实云、安装依赖或推送。历史真机/云
记录只证明当时的源码、配置和测试条件，不自动覆盖本轮修改。
模拟设备的写入记录、驱动返回成功、shadow 和产物构建成功均不证明
物理输出或物理停止时限。迁移状态以
[唯一资产台账](migration-assets.json)为准；这里记录证据，不另设状态源。

## 原审查 20 项反例与整改

下表中的“修复”限定为所述软件反例，不暗示相邻产品合同已完成。
测试文件列出新增或扩充的断言；初始反例还曾在 `84fb440` 上用隔离
探针运行。除特别标明外，表内测试使用 `native_sim` 或离线主机。

| # | 原反例及受影响调用链 | 处理与直接证据 | 范围 |
|---|---|---|---|
| 1 | principal 解绑后复用槽，旧副本或字段不匹配的句柄仍可判权或解绑新主体 | 绑定代数、退役和同步；[R04 测试](../platform/zephyr/tests/r04/src/main.c)测过期、穷尽与消息权限 | 代数只在本次运行有效；C 层完全匹配的伪造仍会通过，见下 |
| 2 | assistant 校验外设描述符后，以别名而非描述符绑定的 safety UID 输出 | 使用受检 `safety_uid`；[assist 测试](../platform/zephyr/tests/assist/src/main.c)验证另一同名通道未被驱动 | `safety_commit` 本身不检查 principal |
| 3 | estop 强停写失败仅停喂 WDT，`clear_fault` 仍能恢复输出 | 升级全局失败并阻止清除；[PWM 测试](../platform/zephyr/tests/safety/src/pwm_real.c)注入强停失败 | 假设备不能证明物理停止 |
| 4 | 时钟回退被夹到旧值后仍被当作可知，另一通道速率门放行 | 以不可知拒绝提交；[safety 测试](../platform/zephyr/tests/safety/src/main.c)覆盖回退与速率基准 | 真实时钟和 ISR 随机交错未穷尽 |
| 5 | KV 满页重启后压实先擦含其他 App 唯一记录的页，数据丢失 | 重启保持活动满页；先读取并确认活动页拥有全部待保留记录，否则拒绝擦除；[store 测试](../platform/zephyr/tests/store/src/main.c)覆盖满页、唯一目标页和故障 | 非安全布局返回 `E_STATE`；维护恢复尚未定义 |
| 6 | KV/Audit 撕裂、损坏或读失败被解释成有效记录/空状态 | 新记录加 CRC、独立提交标记和回读；故障显式传播；[store 测试](../platform/zephyr/tests/store/src/main.c)注入逐阶段写失败、读失败和受保护位翻转 | CRC 非防篡改；旧记录仍可读但无法证明完整性，真断电未测 |
| 7 | Wasm 二次装载在返回 `E_BUSY` 前卸载活模块；验证/读 I/O 被误报为签名/范围错误 | 空闲检查先于修改共享模块，保留底层错误；[Wasm 测试](../platform/zephyr/tests/wasm/src/main.c)再次调用活实例及注入 I/O | 使用固定 WAMR，非其他运行时证明 |
| 8 | Wasm 调用者可替换实例 principal；宽返回类型有单槽缓冲越界风险 | 装载时绑定主体并检查导出 ABI；[Wasm 测试](../platform/zephyr/tests/wasm/src/main.c)测换主体和参数/返回类型 | 试用确认权限已与运行时 natives 授权取交集（WP2）；稳定集准入见下。 |
| 9 | 待确认包可执行；展示字段与确认、装载无关联；诊断自确认冒充用户 | 候选槽在装载/调用时核对状态和摘要，确认核对字段与纪元；[Wasm 测试](../platform/zephyr/tests/wasm/src/main.c)、[试用测试](../platform/zephyr/tests/delivery/src/main.c)覆盖待确认、改字节、过期确认和恢复 | **部分修复**：摘要由设备计算、授权与步数已绑定（WP2）；非候选槽及 `trial_end` 后的稳定集准入（D-E）未实施，纯计算仍可执行但无 natives 权限；串行存储假设下的并发改槽未证明。API 不验证真实用户身份/交互，诊断自动确认不是用户验收 |
| 10 | Forge 组装的 `spin(N)` 在 `N>=1` 死循环，SDK 工厂隔离/预算不符合承诺 | 独立 Forge 工作区修正生成循环并用真实 Wasm 执行、限额负测；当前 WAMR 回归覆盖指令配额 | **部分修复**：该工作区非 Git 仓库、无独立提交锚点；产品隔离、外发限制和 SDK 精确 token 硬上限未证明；SDK 直产替换为确定性组装的方向仍需确认 |
| 11 | 每次模型请求仅计一次却可多次传输；未知 I/O 自动重试；云诊断绕过门；时间窗/冷却失效；HTTPS 等待读取无总预算 | 每次尝试单独过门，仅 `E_NOT_SENT` 自动重试；云诊断接入模型门；修复时间回退、零时刻、冷却及读写共享预算；[assist](../platform/zephyr/tests/assist/src/main.c)、[D12](../platform/zephyr/tests/guard/src/main.c)与离线 HTTPS 故障测试 | 同步 DNS、连接及 TLS 后端内部仍可能阻塞，不能称端到端硬截止；200 次/日已由 WP4a 实现（持久预留，见下） |
| 12 | 复位风暴第 3 次未停，retained RAM 失败被忽略；业务镜像用弱桩或忽略 SAFE-HALT | 更正阈值、故障保守处理，并将保护装入对应镜像；[bootguard 测试](../platform/zephyr/tests/guard/src/bootguard.c)注入读写/复位原因故障 | 连续异常复位不是“一小时内三次”；当前源码的物理复位与 retained RAM 未在真机验证 |
| 13 | WAMR `register_natives` 返回 `false` 仍报告成功 | 按 bool 检查；[Wasm 测试](../platform/zephyr/tests/wasm/src/main.c)注入注册失败 | 生产与测试初始化路径应保持一致 |
| 14 | 带 deadline 的任务在启动前时钟失败仍进入 RUNNING | 未知时钟拒绝启动；[R04 测试](../platform/zephyr/tests/r04/src/main.c)复现后验证 | 已运行任务的实际效果不因取消而撤回 |
| 15 | 网络队列 256 字节帧经 `uint8_t` 长度回调截成零 | sink 长度改 `uint32_t`；[net 测试](../platform/zephyr/tests/net/src/main.c)验证长帧 | 公开 C 回调签名/ABI 改变，调用方需重编译；sink 无回执，不证明远端送达 |
| 16 | 媒体采集中重绑覆盖旧后端，旧采集无法停止 | 采集中拒绝重绑；[media 测试](../platform/zephyr/tests/media/src/main.c)验证旧后端仍可停止 | 真实媒体后端未实现/验证 |
| 17 | 墓碑仅按时间戳过滤，删除后相同时间戳的新写入被吞 | 加写入顺序；[memory 测试](../platform/zephyr/tests/memory/src/main.c)覆盖新旧顺序 | 经历链仍仅在 RAM，不具断电恢复 |
| 18 | affect 中位初值仅测试重置后生效，生产冷启动为零 | 生产静态初值与合同一致；[非测试构建用例](../platform/zephyr/tests/affect/src/cold_start.c)覆盖冷启动 | 非长期体验验收 |
| 19 | Lua 合同 CBOR 字节长度/转义错误并触发严格编译警告 | 修正编码；[pkg 测试](../platform/zephyr/tests/pkg/src/main.c)对拍字节和错误参数 | 现有预算验证是 C 机制桩，不是实际 Lua VM 的资源/中断可行性证明；正式 Lua VM 也尚未交付 |
| 20 | 入口、台账和总结与实现不符；将 RAM 经历、affect reference、配额和 D12 阈值过度报告 | 更新入口、合同、台账与本复核；新增明确的默认 Wasm 配额入口；[Wasm 测试](../platform/zephyr/tests/wasm/src/main.c)覆盖默认与显式限制 | 旧入口 `timeout_ms=0` 不自动变为默认值；历史批次 `done` 不等于现状通过 |

### 本轮追加反例

- **权限重绑与 ISR 载荷**：解绑/绑定与判权竞争曾允许新旧主体混淆；
  ISR 拒绝事件曾借用栈载荷。增加代数同步和小载荷复制，R04 及 Linux
  两条确定性交错测试通过；多核和交错穷尽未证明。
- **原生主体边界**：生产配置、Linux 同源 `perm.c` 的新进程中绑定
  app 42，实际字段为 `(slot=1, gen=1, app_id=42)`；同地址空间直接
  构造这三个值后 `gpio:read:0` 裁决返回 `ENTELEA_OK`。既有负测只覆盖
  不匹配或过期字段，不能证明不可伪造。此探针不是 Wasm 越权证明；
  同地址空间原生调用必须可信，若要承载不可信原生代码，需另定隔离/
  句柄合同，不能依赖字段保密。仓库可选复现：
  `sh platform/linux/tests/run-principal-concurrency.sh <output-dir> native-guess`，
  当前退出 1；该构建含测试钩子但此探针不使用钩子，另用无
  `ENTELEA_TEST` 编译的同源代码得到同样结果。即使以后单个猜测被拒，
  也不能仅据此宣称完整隔离。
- **KV 冲突**：两页同序号有效记录内容不同时不得猜测赢家；独一目标页
  记录不能在压实前被擦。修复前反例失败，store 的 RAM 和模拟 flash
  变体验证修复。介质擦写寿命与真实断电未测。
- **试用持久状态**：未恢复即装载、损坏/缺失/不一致副本曾放开或恢复
  旧状态。首次操作先恢复，两副本必须完整且一致，否则
  `TIMEOUT + UNKNOWN`，两槽受限；中断镜像写入、读错和活候选不被覆盖
  均有测试。落盘仍由装配层显式调用，不是自动持久化承诺。
- **消息名称越界**：非终止名称跨固定字段读入相邻项可能被误授权；
  保留跨项反例布局，校验字段内终止符与长度，R04 12/12。
- **WDT 快照交错**：巡检先取 `now`，ISR 随后更新为更晚的 `last_feed`，
  无符号差值下溢曾误判超时。修复前 core 9/10，实际 `irq_offload`
  注入用例失败；修复后先快照 feed、再取时，快照改变则本轮跳过，
  core 11/11，并覆盖 32 位回绕。仅证明单核确定性交错；halt 时已在飞
  的喂狗调用可能完成，多核竞争未验证。
- **试用纪元耗尽**：构造持久 `UINT32_MAX` 后重推同一候选，旧 epoch 1
  确认曾复活；修复前 delivery 19/20，修复后返回 `E_RANGE` 且不替换
  候选，delivery 20/20。耗尽后的维护策略未定义。

## 权限反例（WP2 后）

原真实 WAMR→native 探针（`TEST_TRIAL_PERMISSION_GAP`：确认权限为 0 时宿主
仍可读 GPIO 授权）已转为默认 wasm 用例
`test_f1_native_grant_is_confirmed_intersect_host`，另有 A1、步数封顶与逐次
授权用例。修复前该探针为 17 项中 16 通过、1 失败；修复后 wasm 套件 25/25
通过，可选 CMake 选项已删除。该结果只证明软件授权路径，不证明真实 GPIO 被访问。

仍未覆盖：签名 manifest 不含权限与步数（D-D 首期不入签，由用户确认与持久
记录约束）；稳定集准入已由 WP3 实施（见下节）；工厂位图到权限列表的映射
（owner 待定）。

### WP2 验证记录（试用授权绑定）

- native_sim（退出码 0）：delivery 22/22、wasm 25/25、r13 2/2、pkg 9/9、r04 12/12。
- 主机：`cmake -S . -B build/rf1-host` 配置与构建，`ctest` 6/6（含 governance_tests、architecture、migration_sources、dependency_manifest）。
- 板级交叉编译（未烧录）：`hw_factory_intake`、`hw_flash_persist` 通过链接；`check_board_ram.py` 为 449,008 与 98,036 字节，红线 552,960。
- 修复前证据：F1 由历史探针 16/17 失败（上节）。F3、F4 及授权路径的修复前可执行对拍未做，因为接口已变更、旧测试不能在新接口上编译；这些修复目前依据代码审阅与 fail-closed 推理，并由修复后的用例覆盖。
- VS Code CMake Tools 入口（WP1）已恢复：选择 kit `GCC 15.2.0 x86_64-linux-gnu` 后，`Build_CMakeTools` 配置并构建通过，`RunCtest_CMakeTools` 6/6 通过（此前扩展报 `no kit selected`）。
- 未测：并发改槽与多核；真实 GPIO 物理效果；真板包加载与试用持久化（`hw_flash_persist` 的试用闸已按决定移出，SRAM 预算下另行规划）。

### WP3 验证记录（稳定集准入，D-E）

- 修复前证据：新增回归断言“晋升须持久结束试用”在首版实现上失败（delivery 22/23
  通过，1 失败：`entelea_trial_state()` 不等于 NONE）。首版只在内存中结束试用，
  未持久化，重启后试用恢复为 ACTIVE，撤销稳定记录对同槽试用无效。修复后该断言通过。
- 修复后（native_sim，退出码 0，双副本 v4 修订）：delivery 24/24（含
  `test_stable_record_corruption_fails_closed`，覆盖单副本损坏、双副本皆损、版本与长度不符）、
  wasm 26/26（含 `test_stable_record_grants_natives_only_for_its_bytes`）、store 33/33、
  r13 2/2、pkg 9/9、r04 12/12。
- 主机：`RunCtest_CMakeTools` 6/6；`check_migration.py --sources` PASS（800 项，117 资产）；
  `unittest discover` 通过；`git diff --check` 无输出。
- 板级（修订后）：`hw_factory_intake` 与 `hw_flash_persist`（esp32p4c5_core_board/esp32p4/hpcore）
  交叉编译通过（未烧录）；静态 SRAM 449,264 / 98,036 字节，红线 552,960（`check_board_ram.py`）。
  meta 缓冲随记录上限增大带来的栈增量未单独测量。
- owner 裁决与实现：①owner 要求双副本 meta（首版误用 KV，已改）：稳定项置于同一 meta
  记录的 v4 稳定区，记录上限 256→448，副本步长与物理布局不变；②计划 D-E 原文“非候选槽默认拒绝”与
  A1 冲突，owner 已确认 `pure_compute_allowed`，故纯计算可执行，仅 natives 需授权。
- 未测：多线程并发晋升与撤销；真实断电注入（当前仅以 `entelea_trial_test_reset` 模拟重启）；
  写入后回读失败的真机复现。残余窗口见[交付合同](../framework/services/delivery/contract.md)“稳定集准入”。

### WP4a 验证记录（D12 日额与平台端口）

- 修复前证据：`calls_total` 仅为自启动计数，`GUARD_CALLS_LIFETIME_MAX` 是每启动上限，
  重启即清零，既无每日 200 上限，也无互斥。新接口下无法编译旧行为对拍，此项以代码审阅为证。
  另：`build/rf1-d12` 残留陈旧 CMake 缓存，导致 `guardretained` 未定义（源码无误）；清洁重配后
  构建通过。此前 14/14 的运行结果来自旧二进制，已作废。
- 实现：`framework/services/config/guard.c`、`framework/include/entelea/guard.h` 按 UTC 日界计
  日额 200；持久记录 12 字节（magic `D12G`、version 1、flags 0、reserved u16、utc_day u32），
  存于 KV 保留键 `0x44313247`；预留步长 K=10，计数达到预留值时先写入 `min(预留+K, 200)`
  再放行；重启后计数取持久预留值（偏保守）；墙钟未知、读失败、记录损坏、写失败均 `E_STATE`；
  日期回退取较晚日、不放宽；入口经互斥端口，未绑定即拒绝，ISR 由锁端口拒绝。
- 平台端口：Zephyr `platform/zephyr/guard_ports.{h,c}`（k_mutex；墙钟在 SNTP 接线前恒未同步）、
  `platform/zephyr/guard_store.c`（KV 适配，仅 `hw_cloud_chat` 链接 `kv.c`）；`boot_steps.c` 绑定，
  持久端口缺席不阻止启动并输出 printk 提示 fail-closed；14 个 boot_steps 目标加入 `guard_ports.c`。
  Linux 宿主 `platform/linux/guard_ports.{h,c}`（pthread、CLOCK_REALTIME、进程内存持久，进程退出后
  不保留）；`cloud_main.c` 绑定；`build-support/build-f2-cloud.sh` 编入。
- 删除：每启动累计上限 `GUARD_CALLS_LIFETIME_MAX`。`entelea_guard_model_calls()` 仅为自启动观测值。
- 测试：`platform/zephyr/tests/guard` 新增 9 个用例（未绑锁、墙钟未知、读失败、损坏记录、写失败、
  掉电重启不重置、每日写入 20 次、墙钟回退不放宽、多线程并发）；`assist`、`wasm` 改用绑定替身与
  真实互斥端口；`platform/linux/tests/guard_concurrency.c` 登记为 CTest `guard_runtime_host`。
- 验证（真实命令与退出码）：
  - native_sim `guard`：清洁重配构建退出码 0；运行 33/33（framework_guard 23 + bootguard 10），退出码 0。
  - native_sim `assist` 12/12、`wasm` 26/26、`core` 11/11、`r04` 12/12、`store` 33/33、`net` 6/6、
    `safety` 7/7、`wdt_start` 5/5、`wdt_hardware` 1/1；构建与运行退出码均为 0，计数与 WP2/WP3 基线一致。
  - 主机 `RunCtest_CMakeTools` 7/7（新增 `guard_runtime_host`）。
  - Linux 云诊断 `sh build-support/build-f2-cloud.sh`：退出码 0（仅编译链接，未运行云程序）。
  - 板级交叉编译（`esp32p4c5_core_board/esp32p4/hpcore`，未烧录）：`minimal` 91,124、
    `hw_wifi_netmon` 280,120、`hw_factory_intake` 449,268、`hw_flash_persist` 98,040、
    `hw_cloud_chat` 481,648 字节；红线 552,960；`check_board_ram.py` 全部 PASS，且与 `size -A`
    输出段求和逐镜像一致（见[资源预算表](board-osptek-p4c5-resource-budget.md#rf1-静态红线复核)）。
- 未测与边界：
  1. 板级墙钟未接 SNTP（owner 已确认留待板级授权批次）：墙钟恒未知，云模型调用 fail-closed，
     `hw_cloud_chat` 等演示不可用。
  2. 掉电由 `reboot()` 与宿主重绑模拟，未在真板注入掉电；KV 磨损按每日约 20 次预留写入估算，
     未以真实 flash 寿命验证。
  3. KV 处于需维护状态（压实返回 `E_STATE`）时 D12 拒绝模型调用；维护恢复策略未定义。
  4. 宿主诊断面的持久为进程内存；产品闸以板级为准。
  5. 未来日期的持久记录会按该日计数（不放宽），时钟异常后额度可能冻结到该日；清理策略未实现。
  6. “连续 3 次异常复位”仍不等于“一小时 3 次”窗口，未实现。
  7. ISR 拒绝由锁端口代码保证，未做中断注入测试；并发仅在主机与 native_sim 线程中验证，未在真板多核验证。
  8. 名称已改为 `guard`（WP4b，提交 30954d1）；改名后的复验见下节“WP4b 改名验证记录”。

### 依赖布局与 Forge 并入验证记录（2026-11-12）

- 决策：所有者表单确认受管检出位于 `.deps/`（Zephyr 经 `west init -l` 绑定固定提交 41d10b5，
  模块经 `west update` 按 west.yml 拉取，修订与 `dependencies.json` 逐项一致）；SDK 迁入
  `.deps/zephyr-sdk-1.0.1` 并以 `ZEPHYR_SDK_INSTALL_DIR` 选择；esp-hosted-mcu 复制入 `.deps/`；
  Forge 并入 `factory/forge/`（推翻 D-M 与 D1 的两仓库约定）；OpenHands 环境迁至
  `/home/emb/project/envs/openhands` 并由带哈希锁同步。legacy 保持冻结原位。
- 缺陷复现与修复：west 位于构建解释器时，Zephyr 继承整个工作区模块集，`build_zephyr.py`
  preflight 报 `Effective module set differs from the verified set`（gate nsim 首次运行失败）。
  修复：west 移出 `.venv`（单独位于 `/home/emb/project/envs/west`，v1.5.0）；`build_zephyr.py`
  新增守卫，`--python` 可导入 west 即拒绝。负测：`--python /home/emb/project/envs/west/bin/python --dry-run`
  退出码 1，未创建构建目录。
- 验证（真实命令与退出码）：
  - 主机 `RunCtest_CMakeTools` 7/7；`python3 -m unittest discover -s tools/tests -v` 退出码 0。
  - `RUN=run004 factory/forge/gate/gate_run.sh nsim`：product、dev、pending 重启三段 `ALL PASS`，退出码 0。
  - Forge 离线预算单测（`envs/openhands` 解释器）16/16 OK。
  - native_sim 构建与运行退出码均为 0，无 `FAIL -` 行，套件均报告 `succeeded`：wasm、guard、delivery、
    store、r13、pkg、r04、assist、core、media、affect、memory、net、safety、wdt_start、wdt_hardware。
  - 板级交叉编译（`esp32p4c5_core_board/esp32p4/hpcore`，未烧录）：`hw_factory_intake` 退出码 0，
    RAM 449,268；`hw_flash_persist` 退出码 0，RAM 98,040；红线 552,960，`check_board_ram.py` PASS。
  - Linux 云诊断 `build-support/build-f2-cloud.sh` 退出码 0（仅编译，未运行）。
  - `python3 tools/check_migration.py --sources`：PASS，800 项，117 资产。
  - 模块与补丁：`check_dependencies.py --dependency zephyr --checkout .deps/zephyrproject/zephyr` PASS；
    `--dependency wamr --checkout .deps/wamr` PASS；esp-hosted-mcu HEAD `2e3f6c8`；SDK `sdk_version` 1.0.1，GCC 14.3.0。
  - OpenHands 环境：版本集合与旧环境逐行一致（仅少 pip）；`openhands-sdk` / `openhands-tools` 1.60.2，`pynacl` 1.6.2 可导入。
- 证据迁出（2026-11-12）：仓库内日志与笔记迁入 `docs/evidence/`（路径与原 `build/` 子目录同名）；
  仓库外二进制与备份位于 `/home/emb/project/backups/`，逐个 `cmp` 核对一致：
  `hw-gate/p4-full-16m.bin` sha256 `c00800dbc24a43c7876c428bf246cd1da3b56cb47b82c4594c81d996cb33b0d6`；
  `hw-gate/legacy-app-0x2000.bin` `faff1a461f7c211f2a47ebe14081090301366c2689f0f32bcce4d36725d652ad`；
  `r02-hwgate/zephyr/zephyr.bin` `b4221a5e54f7153542c3bc90bd0d26cd1272ae60636d7306354904dc56c9fe1e`；
  `r14-tf-final/zephyr/zephyr.bin` `269ee7e25570afcf115fd4941aafb92813d4be1b5c491d21327427fc79b3ae51`；
  `r15-cloud/zephyr/zephyr.bin` `d12b32456ac2db926a03e5a1a4eea468c48db1fbab5e56410d407caac36cd9b0`。
  `build/` 中其余内容均为可再生构建树，删除不影响证据。
- 未测与边界：
  1. `build-support/build-c5-firmware.sh` 未运行（它会对 IDF 副本执行 patch-idf，并覆盖产物与 sha256）；
     仅路径已改，需授权后再执行。
  2. 旧副本 `/home/emb/project/deps/esp-hosted-mcu` 与 `deps/.eh-comp` 已于 2026-11-12 删除（删除前核对 `.deps/esp-hosted-mcu` HEAD 为 `2e3f6c8`）。
  3. 历史验证文档中的旧路径（`../entelea-legacy/.deps/...`）未改写，随历史文档处理。
  4. 板级烧录与真机验证未执行（未授权）；native_sim 结果不代表真板行为。
  5. ruff 版本由 Zephyr 官方依赖固定为 0.14.2，未采用此前计划的 0.17.4。

### WP4b 改名验证记录（30954d1）

- 改名范围：`d12` 模块的文件、目录、公共 API、CMake 选项、DT 标签 `guardretained`、运行时日志前缀 `[guard]` 与 `[guard] SAFE-HALT`、步骤名 `guard_bootguard`、阈值文档 `docs/guard-threshold-proposal.md`。持久魔数（`D12G`、`D12M`、`0xD12`）、哈希与“D12”决策标签保持不变；历史证据日志未改。
- 验证（真实命令与退出码）：
  - 主机 `Build_CMakeTools` 退出码 0；`RunCtest_CMakeTools` 7/7（含 `guard_runtime_host`）。
  - `python3 tools/check_migration.py --sources`：PASS，800 项，117 资产。
  - `python3 -m unittest discover -s tools/tests -v`：109 项 OK。
  - native_sim 16 个套件构建退出码 0，于临时目录运行，均报告 `PROJECT EXECUTION SUCCESSFUL`，无 `FAIL -` 行；`START -` 计数与基线一致：wasm 26、delivery 24、store 33、r13 2、pkg 9、r04 12。
  - 板级交叉编译（`esp32p4c5_core_board/esp32p4/hpcore`，仅编译，未烧录），退出码与静态 SRAM（红线 552,960）：hw_factory_intake 0 / 449,268；hw_flash_persist 0 / 98,040；hw_cloud_chat 0 / 481,648；hw_wifi_netmon 0 / 280,120；minimal 0 / 91,124；tf_probe 0 / 69,068。
  - hw_fault_stop 编译退出码 0，但静态 SRAM 为 640,248，超过红线。以基线提交 55559c2 的导出树复测，同为 640,248，故为既有问题，本次未改变。该应用的 R02 验证记录已删除；owner 决定移除，已从仓库删除（历史见 55559c2）。
  - Linux 云诊断 `ENTELEA_CLOUD_OUT=build/rf1-f2-cloud ENTELEA_MBEDTLS_BUILD=build/rf1-mbedtls-host sh build-support/build-f2-cloud.sh` 退出码 0（仅编译链接，未运行）。
- 排障：hw_cloud_chat 与 hw_wifi_netmon 首次失败，原因是复用的构建目录缓存了旧的 `ZEPHYR_MODULES`（仅 hal_espressif），nanopb 未进入模块集，Kconfig 中止。删除目录后，以 `--module nanopb`（云应用另加 mbedtls、tf_psa_crypto）重建通过。
- 未测与边界：未烧录、未运行板级程序，未做真机验证；native_sim 结果不代表真板行为。

### 文档一致性修订记录（仅文档与注释，不改行为）

逐项对照文档与实现后修订，提交号见台账 RF1 批注。代码行为、测试、持久格式
与构建产物未变；板级 5 个镜像的静态 SRAM 与上表逐字节一致。

- 更正与实现不符的陈述：板级资源预算表与阈值方案的 Wasm 配额（代码上限
  为 1×10^8，没有 2×10^9 钳制）；构建说明中不存在的空白样式检查与构建入口
  引用的 README 小节；本记录里已删除的可选探针参数；Forge README/AGENTS 的
  提交号、`run` 指令编码与失败轮次产物大小。
- 阈值方案新增 §11“落地情况对照”：标明 §5 运行时堆余量、§6 事件丢弃判定、
  §2 停止路径时间戳打印未落地，B9 soak 结果无在树记录，复位保护没有
  一小时窗口。
- 状态文字：台账 M-CONFIG、M-APP、M-DELIVERY、M-TEST、M-FACTORY 与
  R04/R05 备注按 WP2–WP4a 的实现更新；Wiki 的 Forge 位置、`guard` 改名和
  linked worktree 表述（后者仅在原开发机成立；旧 Forge 基线 `ca5219e7`
  未发布到远端）。
- 注释与提示：`commit.c` 文件头、稳定记录的持久方式、假 PWM 周期、测试私钥
  位置、WAMR 垫片版本、云应用 TLS 堆、板级 DTS、`boot_steps.h`；
  `hw_flash_persist` 的复位提示语由“SW reset <=~5s”改为实测的 WDT 复位。
- `.editorconfig` 对 `*.patch` 关闭行尾空白裁剪（6 个补丁含 18 行单空格
  上下文行，字节被 SHA-256 钉死）；`wiki/runtime.md` 重复行与本记录
  反例表的挤行已修。

验证（独立克隆，真实命令与退出码；该环境没有相邻的 `entelea-legacy`）：

- `ctest --test-dir build/r01-venv --output-on-failure`：退出码 8，7 项中 5 项
  通过；`migration_sources` 失败，`governance_tests` 109 项中 105 项通过，
  失败的 4 项与 `migration_sources` 同因（`Git check failed in
  ../entelea-legacy`），属环境受阻，改动前后相同。
- `python3 tools/check_migration.py --sources`：同样受阻，退出码 1；仅元数据
  模式通过（800 条源项、117 个资产）。
- `check_architecture.py`、`check_dependencies.py` 通过；6 个补丁与钉死摘要
  一致；`ruff check` 与 `ruff format --check`（tools 与 factory/forge，18 个
  文件）通过；文档 95 个相对链接无断链。
- native_sim：16 个套件与 5 个构建变体共 21 项全部通过，计数与 WP4b 一致。
- 板级交叉编译（未烧录）：minimal 91,124、hw_flash_persist 98,040、
  hw_wifi_netmon 280,120、hw_cloud_chat 481,648、hw_factory_intake 449,268
  字节，红线 552,960，与上表一致。

本批未改（非文字改动或需所有者裁决）：

1. 阈值方案 §5 运行时堆余量、§6 事件丢弃判定、§2 停止路径时间戳打印的落地。
2. 引导程序未定型：`Kconfig.sysbuild` 缺省 MCUboot，`image.cmake` 仅在
   simple-boot 下生效，应用目录没有 sysbuild 文件。
3. estop 仅在 `minimal` 的 overlay 绑定；`hw_flash_persist`、`hw_wifi_netmon`
   未链接启动保护。
4. `tests/contracts/main.c` 第 273、274、278 行的断言不经过框架代码；21 个
   公共函数没有测试直接调用。

### 质量债批次验证记录（仅测试与检查，不改固件行为）

按规划 10.6 的裁决补测试与检查，提交号见台账 RF1 批注。生产源码没有行为
改动：`package.h` 只做了 clang-format 的换行归位，板级 5 个镜像的静态 SRAM
与上表逐字节相同。

恢复与新增的检查：

- `tools/check_architecture.py` 补回四条旧 L5 机械安全规则，只扫生产源码：
  通道描述符必须声明三个安全态；estop 入口函数体与保护驱动源不得出现分配、
  锁、队列、`printk` 或协议栈符号；provisioning 装载源不得有存储写调用；
  包槽分区标识只允许出现在存储端口、包存储及其公共头。各配负测与放行用例。
  旧 L5 的常量出处注释规则和内核 checkpatch 严格门没有恢复：前者依赖旧出处
  词汇（77 个数值常量里 52 个不满足），后者在当前源码上有 951 条发现，主要
  是块注释与 SPDX 注释约定。两者都是需要所有者裁决的风格约定。
- 新增 `tools/check_style.py`：clang-format 23.1.5（实测与现有布局一致，
  其他版本被拒绝）对全部第一方 C/H 做只检查，加受跟踪文本文件的空白卫生
  检查。豁免被钉死字节的补丁、上游 TweetNaCl 源码、生成的工厂产物和取证
  日志。`package.h`、`tests/assist`、`tests/guard`、`tests/contracts` 四个
  此前没有按该版本归位的文件已格式化，前三个逐个标记不变。

新增与修正的测试。native_sim 合计由 247 增至 269 个用例（含变体）；默认
配置的 16 个套件由 196 增至 209：

| 套件 | 之前 | 之后 | 新增内容 |
|---|---|---|---|
| safety / safety-pwm | 7 / 14 | 12 / 19 | 按状态计数；输出值编码与写记录对拍；未绑定的硬件后端显式报告缺席；持锁提交；estop 事件每次闩锁恰补发一次并携带触发时刻；安全态变化事件 |
| pkg | 9 | 11 | 包槽直接读取；装载门依赖的流式 SHA-256 与公共库的另一实现在多分块包上对拍；单趟捕获窗口 |
| store / store-flash | 33 / 32 | 37 / 36 | 各分区大小与共享端口的隔离读取、越界拒绝；provisioning 校验不改已装载状态；失败原因钩子的留痕；六步启动表真实执行到 `BOOT_DONE` |
| r04 | 12 | 13 | 权限列表解析：定长输入、空列表、畸形列表整体拒绝 |
| guard | 33 | 34 | 日额经生产 KV 适配器（`guard_store.c`）持久：落盘、掉电后仅凭记录重建、用尽后重启仍用尽、跨日恢复 |
| 宿主 `common_contracts` | — | — | `tests/contracts` 中三处不经过框架代码的断言，换成运行时种类、计量单位、效果与调用状态取值的钉定，App 结构布局的静态断言，以及预算字段上“无截止时间不等于 0 毫秒截止时间”的时间契约行为 |

此前列出的 21 个没有测试直接调用的公共函数，按裁决只补安全、存储、配置与
授权相关的；本批之后其中 14 个有了直接调用，仍未被任何测试引用的 7 个是
`entelea_be16`、`entelea_be32`、`entelea_tsap_parse_fields`、
`entelea_periph_count`、`entelea_power_budget_ma`、
`entelea_power_used_ma`、`entelea_pubq_pop`。

验证（独立克隆，真实命令与退出码；该环境没有相邻的 `entelea-legacy`）：

- native_sim：16 个套件与 5 个构建变体共 21 项全部通过，269 个用例 0 失败。
- 板级交叉编译（未烧录）：5 个镜像全部通过，静态 SRAM 与上表相同。
- `ctest --test-dir build/r01-venv --output-on-failure`：退出码 8，7 项中 5 项
  通过。`migration_sources` 与 `governance_tests` 的失败原因与上一节相同，
  属环境受阻；`governance_tests` 由 109 项增至 126 项。
- 带 ASan/UBSan 的宿主配置：`common_contracts` 与 `guard_runtime_host` 通过。
- `python3 tools/check_style.py`：通过，145 个第一方 C 源与 clang-format
  23.1.5 一致，空白卫生无发现。`python3 tools/check_architecture.py`：通过。
- `ruff check` 与 `ruff format --check`（tools 与 factory/forge，20 个文件）、
  `check_dependencies.py`、`check_migration.py`（仅元数据）通过；
  `check_migration.py --sources` 仍受阻，退出码 1。
- Forge 离线预算单测 15 项，1 项因无用量台账跳过。

本批发现但没有改的问题：

1. 存储的测试复位钩子 `entelea_storage_test_reset()` 只擦分区，不清 noinit
   模块缓存的上次复位原因；而 ztest 按用例名排序执行。既有的
   `test_system_fail_reason_persisted` 此前只是碰巧排在前面才通过，新用例
   因此刻意不调用 `entelea_storage_init()`。是否补全复位钩子待裁决。
2. 运行时种类有两套编码并存：`app.h` 的 `entelea_runtime_kind_t`
   （Wasm=1，Lua=2）与工厂候选结构的 `payload_kind`（0=wasm，1=lua）。
   没有发现混用，但属于易错点。
3. `entelea_perm_parse_list` 对空列表不清权限表，依赖调用方先清零。三个
   生产调用方都这样做，已由用例钉住。
4. 事件总线没有退订接口，订阅是进程级的静态资源；新增订阅者的测试要避开
   其他用例的容量断言，启动用例因此放在 store 套件而不是 core。

### 环境固化批次验证记录（仅新增工具与文档，不改固件行为）

按规划 10.6 的裁决，把在新机器上重建环境的过程固化为可复现的入口；提交号
见台账 RF1 批注。

- `build-support/setup-env.sh`：幂等重建 `.venv`、west 环境、带批准补丁的
  Zephyr、四个固定模块、WAMR/Lua/esp-hosted-mcu 和 Zephyr SDK 1.0.1。
  每一步用 `tools/check_dependencies.py` 核验；提交号和补丁只取自
  `dependencies.json`，没有回退到未固定来源的路径，west 不进入 `.venv`。
- `build-support/requirements-zephyr.lock`：`.venv` 的带哈希锁（CPython
  3.12、Linux，167 个包，west 被排除），取代原先现场解析依赖的做法。
  clang-format 23.1.5 与 `tools/check_style.py` 要求的版本由单测交叉核对。
- `tools/tests/test_setup_env.py`：8 项治理单测（脚本语法与帮助、未知选项
  拒绝、提交号不得写死在脚本里、west 不入锁、每个钉定的包都有哈希、锁头部
  的再生成命令）。
- 构建说明新增“搭建开发环境”，写明此前没有记录的前置条件：32 位编译支持、
  主机工具、uv，以及 Zephyr 构建须把 `.venv/bin` 放在 PATH 最前。

验证（真实命令与退出码）：

- 幂等重跑：在现有环境上 `setup-env.sh` 退出码 0，各阶段识别为已就绪并完成
  核验。第一次重跑暴露并修正了脚本的两个缺陷（补丁列表的读取、`west list`
  的参数），上述结果是修正之后的。
- 从零重建：把仓库克隆到临时目录，`setup-env.sh --proxy <镜像前缀>` 退出
  码 0，全部阶段通过检查器。随后在该环境里：
  - native_sim `safety` 套件构建与运行退出码 0，12/12；
  - 板级 `hw_wifi_netmon` 交叉编译退出码 0（经过 nanopb 生成和 RISC-V
    工具链），静态 SRAM 280,120 字节，与本记录中的数值相同；
  - 宿主 CTest 5/7，失败的 2 项原因同前；`.venv` 中 west 不可导入。
- 锁：对现有 `.venv` 带哈希安装的结果是“无改动”；在全新环境按锁同步后
  关键模块可导入，clang-format 23.1.5、ruff 0.14.2。
- `unittest discover -s tools/tests`：134 项，失败的 4 项原因同前。

没有实测的两处：`--system`（用 apt 安装主机包）没有在脚本里跑过，因为本机的
主机包在脚本写成之前已经装好，它只是一条 `apt-get install`；直连 GitHub 的
路径也没有在脚本里跑过，本机直连约 20 KB/s，测试走的是镜像前缀。

## 对照原规划和批准修订

| 批次 | 当前范围与未完成项 |
|---|---|
| R00–R01 | 来源分类、公共原语和台账检查已有；入口状态已纠正。历史治理测试不是固件行为证明。 |
| R02 | 输出、安全态和监督机制已有并修复上述反例；单核确定性交错有证据。真实输出波形、定量停止延迟、WDT 复位和长期运行在当前源码未验收。 |
| R03 | KV、Audit、meta、信任表及配置机制已有；旧记录完整性、真实断电/磨损与不安全布局维护仍有限制。 |
| R04 | 身份、能力、资源、任务和仲裁机制已有；权限表由可信宿主绑定，不能由试用位图自动推导；同地址空间原生代码不在句柄防伪边界内。 |
| R05 | TSAP 签名与单实例 WAMR 有机制测试；Lua 首期需要合同和关键可行性验证，现有 C 预算桩未证明真实 Lua 资源/中断，正式 VM 未交付；产品级配额/签名事实绑定仍待完成。 |
| R06 | 网络机制与依赖状态机已有；队列发送无远端回执，真网历史记录不代表本轮验收。 |
| R07 | Linux 现为共享机制合同宿主及诊断，依 R-V5 不等于完整 Linux Wasm/App 产品；ARM64 未实跑。 |
| R08 | 有辅助决策、受检输出和模型适配机制；不是完整常驻认知产品；同步 HTTPS 非硬截止。 |
| R09 | 有媒体接口和假后端；真实语音/视觉/显示及 D11 闸未完成，接口不等于设备功能。 |
| R10 | 有同意、删除和有界 RAM 经历；原规划的可恢复持久经历链尚未交付，不能仅归因于 schema 待定。 |
| R11 | 有模拟 affect 机制；台账中的 20 个旧源为 reference，不是 20 个已迁移源，真实长期行为未验收。 |
| R12 | 试用状态、候选摘要（设备计算）、授权与步数绑定，以及稳定集准入（D-E，WP3）已有；Forge 的版本化与隔离尚未证明。 |
| R13 | 配置矩阵和两条代表性机制断言不等于完整框架或产品验收；依赖 RF1 及未闭合决策，保持 pending。 |

R-V1–R-V5 是原规划的批准范围修订，不能反推其余原要求已交付。
追加 r14–r16 的板级、云和阈值记录保留为历史证据；D12 是设计
决策编号，不是行业标准，实际 `guard` API 的兼容重命名需单独设计。

## 本轮实际验证

- 固定环境：Zephyr `41d10b59`、WAMR `25bd7eb`、板级 SDK 1.0.1 /
  RISC-V GCC 14.3.0；只读使用冻结依赖和已有虚拟环境。
- 16 个 `native_sim` 测试应用基础回归通过；最后一次 WDT/纪元修改后，
  重新运行受影响的 core 11、R04 12、assist 12、net 6、R13 2、
  safety 7、store 33、wdt_start 5、wdt_hardware 1、delivery 20、
  Wasm 16；各数值为通过用例数。D12 包括运行闸 14 和 bootguard 10。
- 变体：模拟 flash store 32、假 PWM safety 14、生产 affect 冷启动 1、
  必需 WDT 缺失 3、WDT 设备未就绪 1、WiFi 诊断 7、云诊断装配 8。
  首次 flash 变体漏加 `prj-flash.conf` 链接失败，补齐后通过。
- 离线主机：principal 判权与消息名单两种确定性交错、netmon 并发、
  P4C2 ASan/UBSan、Linux 云诊断 6 项通过；Linux 和 Zephyr HTTPS
  各 46 种故障模式通过。未连接网络或调用真实云。
- 板级仅交叉构建 minimal、hw_factory_intake、hw_flash_persist、
  hw_wifi_netmon、hw_cloud_chat；对每个 map 独立运行 RAM 检查并检查
  退出码，再与该 ELF 的 `size -A` 输出段对拍。数值见
  [资源预算表](board-osptek-p4c5-resource-budget.md#rf1-静态红线复核)，
  不代表动态堆/栈峰值或真机行为。五个产物配置均为
  `CONFIG_MP_MAX_NUM_CPUS=1`、`CONFIG_SMP` 未启用；不能外推多核。
- 原审查曾对 `84fb440` 运行 CTest 6/6；本轮 VS Code CMake/CTest
  专用入口返回 `Unable to configure the project`，没有本轮 CTest 通过
  证据。`python3 -m unittest discover -s tools/tests -v` 通过 106 项；
  `check_architecture.py` 检查 85 个分类 C 源，依赖元数据检查通过，
  更新台账后 `check_migration.py --sources` 核对 800 条源项、117 个
  资产组；治理单测也再次通过 106 项。

### 复现配置

以下为本轮使用的基本命令形态；构建产物写在新树 `build/`，测试程序
在独立临时运行目录运行，避免覆盖工作区中的持久化文件。路径、依赖
固定版本和具体测试参数是证据的一部分：

```sh
cd /home/emb/project/entelea
/usr/bin/python3 tools/build_zephyr.py --app platform/zephyr/tests/delivery \
  --board native_sim --zephyr-base .deps/zephyrproject/zephyr \
  --python .venv/bin/python3 \
  --build-dir build/rf1-delivery-epoch-after-fix --build
# 在独立临时目录运行 build/rf1-delivery-epoch-after-fix/zephyr/zephyr.exe
```

模拟 flash 变体另加 `--cmake-arg=-DTEST_FLASH_BACKEND=ON` 和
`--cmake-arg=-DEXTRA_CONF_FILE=prj-flash.conf`。板级使用
`esp32p4c5_core_board/esp32p4/hpcore` 和对应精确模块，构建后分别运行
`tools/check_board_ram.py <map>` 与 RISC-V `size -A <elf>`；构建脚本经
管道摘要的 RAM 状态不可替代独立退出码。

## 未完成与决策

1. 工厂产物权限位图到权限列表的映射待 owner 决定（`hw_factory_intake`
   当前以空列表提交）。`hw_flash_persist` 的试用持久化闸已按决定移出
   （SRAM 预算），板级恢复须在获批预算后另行规划。
2. 规划持久经历 schema、真实介质恢复和满盘策略；明确试用数据晋升、
   双副本异常与纪元耗尽的维护流程。
3. 每日限额与跨重启持久已由 WP4a 实现（板级墙钟待 SNTP 接线）；仍需确定板级可信时基、一小时复位窗口及端到端
   超时语义。
4. 逐项决定 D11 真实媒体、Linux 完整宿主/ARM64、Lua VM、
   Forge 产品隔离/版本锚点及 SDK 产物方向的后续范围；若需要不可信
   原生扩展，同步决定进程隔离及身份传递边界。
5. 硬件授权后单独规定负载、测量仪器、触发条件与停止阈值，再验证
   当前源码的物理停止、真复位、掉电与长期运行。历史日志不足以
   代替此验收。
