# R02 最小启动与输出保护验证（进行中批次：软件路径）

本批授权边界：R02 已获批开工；硬件闸（D12：参考板/负载/测量方法/
停止阈值/安全状态定义）未冻结，物理验收保持 blocked。本记录只覆盖
软件路径与 native_sim 证据。

## 交付范围（本轮软件闭环）

- 构建入口 `tools/build_zephyr.py`：显式 ZEPHYR_BASE/SDK/工具链前置
  核验（复用只读依赖核验器），native_sim 显式 host 工具链、板级显式
  SDK 版本+目标编译器实测版本；缺失/不匹配/缓存冲突显式失败，
  不自动下载或回退；每目标独立 build 目录，`ZEPHYR_MODULES` 显式空，
  不用 west/旧缓存。
- 监督模块（framework/runtime/supervision）：同步事件总线（静态序、
  ISR 队列+借用寿命约束）、软 WDT（注册/激活/豁免/巡检/task_wdt L3）、
  监督启动执行器（顺序=规格、任一步失败 fail-safe）。
- 输出保护（framework/execution/safety）：三安全态通道注册表、唯一
  写路径（限幅/slew/限流/末段临界区）、estop/fail-safe 直达与补发、
  唯一驱动调用点 drivers.c（source-policy 保护门非空后的机械检查）。
- 平台装配（platform/zephyr）：uptime 时钟适配（ISR 安全原始读）、
  启动表（estop→poweron→wdt→ready，尾部追加规则）、最小监督镜像、
  4 个 ztest 应用（core/safety/wdt_start/wdt_hardware，含 2 个变体）。

延后子集（记录于合同与台账，不是行为声明）：审计环形+留痕持久化→R03；
控制源仲裁→R04；NET/APPMGR WDT 源、estop prov 边沿、通用 GPIO 输入
后端→随各自批次；A-BOARD 板级迁移与板级构建→参考板冻结后。

## 真实验证结果（2026-11-08，本机）

工具链与来源：host cc = Ubuntu GCC 15.2.0；Zephyr 4.5.0 固定
checkout（六补丁完整核验通过）；Zephyr 构建解释器显式使用冻结旧
`.venv` 的 Python 3.12.13（只读复用，含 pyelftools；系统 /usr/bin/python3
缺该包且不安装共享工具）；native_sim 板级；SDK 前置核验对
`/home/emb/zephyr-sdk-1.0.1` 实测（sdk_version=1.0.1、RISC-V GCC 14.3.0）。

- 主机治理回归：check_architecture PASS（19 源恰分类；保护门仅
  drivers.c）；Python 单测 70 项通过（R01 48 + R02 新增 22：构建入口
  15 项负/正测 + 架构门 7 项更新/新增）；CMake/CTest 6/6 通过。
- 构建入口真实负测（非 mock）：错误 sdk_version、板级缺 SDK 参数、
  build 目录缓存他目标 → 全部显式 FAIL 退出 1；板级正确组合 dry-run
  解析出真实 SDK/编译器版本。
- native_sim 构建 7 个目标全部经新入口完成；运行 10 份执行：
  - minimal：启动→横幅→安全起始态→常驻空闲（native_sim 5s 截止）。
  - core 套件 4/4：订阅静态序/容量、ISR 队列+静态载荷寿命、时钟绑定
    E_UNINIT、WDT 数学（激活/逾期/警告/豁免窗/钳制/去激活）、
    system_fail 可观、启动顺序守卫。
  - safety 套件 3/3：注册校验、三安全态迁移、限幅/slew/限流、detach/
    recover、ISR 拒绝、容量 32、estop 锁存/释放/再锁存、poweron 落驱动
    恰一笔、estop 边沿映射、无节点板 estop init 空操作。
  - wdt_start 1/1 ×2 变体：task_wdt init/add 失败不报告成功、不分配
    通道、不置 started、重试不重复初始化；必需硬件缺失变体在 task_wdt
    面之前显式 E_IO。
  - wdt_hardware 1/1 ×2 变体：假 wdt 设备后端错误不报告成功/不被喂、
    boot 第 3 步失败后不执行后续步骤（BOOT_STEP=3、无 DONE、
    FAIL_BOOT(2)、通道 SAFE_FAULT、poweron 输出不保留）、boot 线程
    停机挂起（join -EAGAIN）、成功启动被喂、重复启动 E_STATE、通道
    不泄漏；not-ready 变体阻塞启动且零安装/零喂狗。

## 未测与边界

- native_sim 只证明软件路径：上电/复位/失联/物理故障停止、真实 WDT
  硬件、estop GPIO 物理边沿未测（D12 冻结后硬件闸另测）。
- 板级（ESP32-P4/OSPTEK）构建未做：A-BOARD 未迁移、参考板未冻结；
  SDK/工具链组合已通过 dry-run 前置核验但不等于板级编译通过。
- PWM 真后端/pwm_err 计数未在 native_sim 实测（无 PWM DT 绑定路径），
  仅桩路径与编译面覆盖。
- 事件 ISR 载荷静态存储约束无机械检查（评审+用例示范承担）。
- 未运行：sanitizer 目标、旧基线重跑、真实网络/云/硬件/烧录。

## 复现

```sh
python3 -m unittest discover -s tools/tests -v
python3 tools/build_zephyr.py --app platform/zephyr/apps/minimal \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --cmake-arg=-DPython3_EXECUTABLE=/home/emb/project/entelea-legacy/.venv/bin/python3 \
  --build
./build/r02-native_sim/zephyr/zephyr.exe   # Ctrl-C/timeout 截止
# 测试应用同理：tests/{core,safety,wdt_start,wdt_hardware}
# wdt_start 变体：--cmake-arg=-DTEST_WDT_REQUIRED_MISSING=ON
# wdt_hardware 变体：--cmake-arg=-DTEST_WDT_HW_NOT_READY=ON
```
