# R02 最小启动与输出保护验证（进行中批次）

授权边界：R02 软件整改已按 owner review 确认的方案实施（缺陷 1–6，提交
`a2fa264`）；D12 硬件闸（参考板/负载/测量方法/停止阈值/安全状态/panic/
estop DT）未冻结，物理验收保持 blocked，本记录只覆盖软件路径与
native_sim 证据。修复前每项缺陷有对应的“修复前失败”负测（见矩阵）。

## 交付范围

- 构建入口 `tools/build_zephyr.py`：显式 ZEPHYR_BASE/模块/SDK/工具链/
  板根前置核验；受管 CMake 变量不可被 `--cmake-arg` 覆盖；native 绑定
  已核验编译器；受管 `--python` 保持 venv 语义（absolute 不 resolve）。
- 监督模块 `framework/runtime/supervision`：事件总线（静态序、ISR 队列、
  借用寿命约束、时钟单调护栏）、软 WDT（激活/豁免/巡检/task_wdt L3/
  **一次性全局停机 halt**）、启动执行器（顺序=规格、失败 fail-safe）。
- 输出保护 `framework/execution/safety`：三安全态注册表、唯一写路径
  （限幅/slew/限流/末段临界区/**后端失败传播**）、estop/fail-safe
  （**halt 先行**）、模块私有驱动表（**全库唯一输出调用点**，机械禁旁路）、
  **PWM u==0=占空 0 物理停止**。
- 平台装配 `platform/zephyr`：uptime 时钟适配、启动表、最小镜像、
  5 个 ztest 应用（core/safety×含 PWM 真路径变体/wdt_start×2/wdt_hardware×2）。

延后子集（记录于合同与台账）：审计环形+留痕持久化→R03；控制源仲裁→R04；
NET/APPMGR WDT 源、estop prov 边沿、通用输入后端、墙钟→各归属批次；
A-BOARD 迁移与板级构建→D12 冻结后；testcase.yaml（twister）defer。

## 验收矩阵（整改后按此评估；“测试通过”只是执行结果描述）

| 承诺 | 保证链 | 破坏条件→负测（修复前失败） | 未验证范围 |
| --- | --- | --- | --- |
| 唯一写路径 | commit/三态迁移/poweron/force→模块私有 `entelea_drivers`（internal.h） | 模块外引用→架构检查拒绝（合成源负测）；同链接手写 extern **不能机械防**，评审承担 | 链接级符号滥用 |
| 后端失败不报成功 | write 返回值→E_IO/影子与 golden 不更新/poweron 聚合失败 | 假 PWM 注入失败→commit E_IO、readback/sim 记录不变（修复前：返回 OK 且影子已更新） | 真机 GPIO/PWM 物理失败 |
| PWM 故障值=物理停止 | u==0 解码占空 0（固定 1s 周期脉宽 0） | 假 PWM 断言 pulse==0（poweron/显式提交/force 三面；修复前：hz==0 拒绝落驱、保持原输出） | 真机波形/停止延迟 |
| estop 不被覆盖 | 写+影子+状态全在 irq_lock 内；发布前复查 forced | force 后 poweron/set_link/recover 守卫用例（修复前：poweron 覆盖 SAFE_FAULT） | 真实 ISR-线程随机交错（结构闭环，未做交错模糊） |
| 故障=全局停机 | system_fail 先置一次性 halt→巡检不喂不排 | 非巡检线程 fail 后 tick=false、task_wdt 零喂（wrap 计数；修复前：巡检继续喂） | 真机 WDT 复位闭环 |
| 时间失败/回退不放宽限制 | 三模块单调护栏+失败计数；commit now<last_t 拒绝 | 一次性失败/回退注入→事件戳不回退、巡检不伪逾期、slew 界不放大（修复前：dt 下溢界巨大，提交“成功”落 300） | — |
| 构建配置不可偷换 | 受管变量黑名单+受管注入+绑定已核验编译器 | `-DBOARD=`/`-DCMAKE_C_COMPILER=` 真实拒绝（exit 1）；native 计划含 CMAKE_C_COMPILER | — |
| 启动顺序=规格 | 平台表守卫用例 + boot 失败集成（线程停机挂起、无后续步骤） | wdt_hardware 集成（fail reason/BOOT_STEP 计数/输出不保留） | — |
| D12 硬件闸 | — | — | 保持 blocked：真实负载、物理测量手段、触发条件、停止阈值未定；串口正常≠物理停止 |

## 真实验证结果（2026-11-08，本机；整改提交 `a2fa264`）

环境：host cc=Ubuntu GCC 15.2.0（入口显式 `-DCMAKE_C_COMPILER=/usr/bin/cc`
绑定）；Zephyr 4.5.0 固定 checkout（六补丁只读核验通过）；构建解释器
`--python ../entelea-legacy/.venv/bin/python3`（Python 3.12.13，含
pyelftools/jsonschema；系统 python 缺包且不安装共享工具）；native_sim。

主机（全部实际执行）：
- `python3 tools/check_architecture.py`：PASS（20 源恰分类；输出门=
  drivers.c；`entelea_drivers` 模块外引用拒绝）。
- `python3 -m unittest discover -s tools/tests`：85 项 OK（构建入口 30 项
  含 F6 黑名单/受管注入负测；架构 27 项含驱动表旁路正负测）。
- `python3 tools/check_migration.py --sources`：PASS（800 项源核验）。
- CTest（build/r01）6/6。

native_sim（全部经新入口，8 构建目标 13 次执行）：
- minimal：启动→安全起始态→常驻空闲（5s 截止，无重启）。
- taskwdt 预演：10.03s 单次启动零通道复位。
- core 5/5：订阅序/容量、ISR 队列+静态载荷、时钟护栏（失败/回退不伪
  逾期、计数可观）、WDT 数学（激活/逾期/豁免/钳制/去激活）、system_fail
  可观、启动顺序守卫。
- safety 基 4/4：注册校验、三态迁移、限幅/slew（含 dt=0 护栏界）/限流、
  detach/recover、ISR 拒绝、容量、estop 锁存/释放/再锁存、
  **poweron 保持 estop 锁存（F3）**、poweron 落驱动、边沿映射。
- safety PWM 变体（TEST_PWM_REAL）6/6：基套件不受真后端影响 +
  **u==0 停止（pulse==0×3 面）**、**后端失败 E_IO/影子与 golden 不变**、
  poweron 失败 E_IO、force 失败计数（F2）。
- wdt_start 3/1：task_wdt 失败不报成功（×2 变体）；**halt 后 tick=false、
  200ms 零喂、start 拒绝；system_fail 全局零喂（F4）**（默认变体）。
- wdt_hardware 1/1×2：假 wdt 设备后端错误不报成功/不被喂、boot 第 3 步
  失败无后续步骤、线程停机挂起、**boot 内 system_fail 后 tick=false（F4）**、
  成功启动被喂、通道不泄漏；not-ready 变体零安装零喂。

修复过程实录（针对性验证先失败后通过）：时钟护栏场景修正（初次场景设计
错误：一次性失败后第二次读看到真实前进时间，逾期是正确行为——已改为
界内前移+回退不 feed 场景）；wdt 测试复位须先同步取消已调度巡检
（对调度中 delayable 重复 init 是 UB，曾致挂起）；wdt_hardware boot 集成
段移至末段（system_fail 的 halt 一次性，其后不得再 start）。测试钩子
（tick/reset/注入）仅 ENTELEA_TEST 构建可达，不绕过被验收路径。

## 未测与边界（如实）

- native_sim 只证明软件路径：上电/复位/失联/物理故障停止、真实 WDT/
  estop GPIO、PWM/GPIO 真后端物理效果未测（D12 冻结后硬件闸）。
- 板级（OSPTEK ESP32-P4）构建未做：A-BOARD 未迁移、参考板未冻结；
  ESP_SIMPLE_BOOT + prepare_p4_image.py（清单内）路径已核查，SDK 组合
  dry-run 通过，不等于板级编译/运行通过。
- estop ISR 与线程的真实随机交错未做模糊测试（临界区结构闭环+守卫用例）。
- 事件 ISR 载荷静态存储约束无机械检查（评审+用例示范承担）。
- 本记录不覆盖的早期提交（`5c5e2aa`–`7893aa4`）的整改前状态：其时
  “软件闭环完成”结论已被 owner review 否定并撤回，缺陷清单与本整改
  一一对应。

## 复现

```sh
python3 -m unittest discover -s tools/tests -v
python3 tools/build_zephyr.py --app platform/zephyr/apps/minimal \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r02-native_sim/zephyr/zephyr.exe
# tests/{core,safety,wdt_start,wdt_hardware} 同理；变体：
#   safety: --cmake-arg=-DTEST_PWM_REAL=ON（PWM 真路径）
#   wdt_start: --cmake-arg=-DTEST_WDT_REQUIRED_MISSING=ON
#   wdt_hardware: --cmake-arg=-DTEST_WDT_HW_NOT_READY=ON
# 负测演示：--cmake-arg=-DBOARD=qemu_x86 → 显式 FAIL（exit 1）
```
