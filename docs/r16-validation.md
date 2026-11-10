# r16 验证记录：D12 阈值落地（[B] 项工程批 + [A] 冻结确认）

owner 2026-11-10 批准 d12-threshold-proposal v1 全包（"同意"）。

## 落地清单（[B] 全项）

| # | 项 | 实现 | 验证 |
|---|---|---|---|
| B1 | 静态 RAM 红线 ≤540KB | tools/check_board_ram.py（map HP SRAM 段求和） | r15-cloud=520KB PASS；**逼出 minimal RAM 镜像数组债（655KB FAIL）→ 板级 minimal 切 flash 后端+NO_RAM_BACKEND 后 PASS**；r16-minimal PASS |
| B2 | wasm 默认配额 1e8 | wasm_host quota clamp 100000ms 档 | 构建绿（wasm 套件复跑绿） |
| B3 | 事件丢弃阈值 | 观测计数已有（events dropped）+ 验收闸判据面 | 闸判据=soak/验收批断言 dropped==0 |
| B4 | 429 冷却 60s | d12.c E_BUSY→cooldown | tests/d12 ztest 5/5 |
| B5 | 频度 6/60s+200/启动 | d12.c 滚动窗+终身限 | 同上 |
| B6 | 连续 3 败风暴闩 | d12.c streak+latch+外部 clear | 同上 |
| B7/B8 | 异常复位循环保护+冷却窗 | platform d12_bootguard（__noinit+hwinfo；≥3 连异常→SAFE-HALT 常驻喂狗）+boot_note 60s 窗 | ztest 逻辑面+boot 步骤接入（core/store 断言 6 步） |
| B9 | 24h soak | r16-minimal=soak 镜像已常驻（estop lvl=1 心跳） | **数据 24h 后收**（判据：无 FATAL/无 WDT 复位/心跳连续） |

新组件：framework d12.h/d12.c（fail-closed 闸，ztest 5 用例）、
platform d12_bootguard.c、tests/d12 套件、boot_steps 首步 d12_bootguard
（弱符号；SAFE-HALT 判定归装配层——不进 system_fail 防 MWDT 复位反哺
风暴）。model_ask/wasm_call 前置接入；assist/wasm 测试补绑时钟。

## 真机验证状态（如实）

- r16-minimal 烧录：boot 6 步过、estop lvl=1、RAM 红线 PASS、
  常驻心跳=soak 启动。
- **storm 触发链真机演示 PASS（2026-11-10/11）**：STORM_DEMO 变体
  （minimal 条件编译：system_fail→WDT 停喂→MWDT 复位）自动循环——
  abnormal #1/#2/#3 计数跨复位累加（LP retained RAM）→ #4 启动
  RESET-STORM SAFE-HALT 常驻喂狗。证据
  build/r16-evidence/storm-demo-RESET-STORM.log。
  **攻坚副产物（三连坑，全真机实证）**：①__noinit 宏不达段
  ②.dram0.noinit 带 load address=flash 装载恢复段（不跨复位，读值
  固定=b9b6a4cd 证据）③storage noinit 槽与 last_reset 留痕单槽互踩
  ——终解=LP 域 retained RAM（retained_mem API；POR 自然清=风暴窗
  出）。变体经 --cmake-arg=-DSTORM_DEMO=ON（默认不编译）。

## 回归

16 ztest 套件（+d12 新 1）+ wdt_hardware + CTest 6/6×2 + host +
checkers + policy 77。板终态=soak 镜像常驻。

## [A] 冻结确认

提案 [A] 值随本批生效（零改动面）；放宽需新实测数据规则不变。
[B] 剩余=storm 真机演示+soak 数据回收（24h）→ R13 产品验收闸
可测断言集就绪。
