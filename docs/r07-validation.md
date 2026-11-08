# R07 Linux 最小宿主验证

授权边界：D02 已批准方向（最小原生宿主+同源合同+Wasm 证明；完整 Linux
伴侣延期）。R07 depends_on R01/R04/R05（软件面已交付）。

## 交付范围

- `platform/linux/`：POSIX 时钟适配（CLOCK_MONOTONIC，独立 clock_id=2）
  + 同源合同矩阵主机二进制 + Zephyr→POSIX 垫片（pthread mutex 映射
  struct k_mutex / ISR 恒 false / 工作调度 no-op——巡检经测试直调）
  + 桩头目录（kernel/devicetree/gpio/util/atomic——框架源零修改）。
- `build-support/build-linux-host.sh`：受管构建入口（-Werror）。

## 验收矩阵与真实结果（2026-11-08；HOST SUITE PASS 实跑）

| 承诺 | 证据 |
| --- | --- |
| 同源纪律（一套源码两侧平台） | 框架源零修改（垫片/桩在 platform/linux） |
| 时钟（实例/单调/clock_id） | now.valid + clock_id==2 + ms 单调不回退 |
| 事件总线（静态序同步分发） | 双订阅按序执行（order==2） |
| 安全机制（三态/唯一写路径/锁存） | POWERON 拒→link→ACTIVE→commit OK→readback→estop→SAFE_FAULT+锁存期拒 |
| fail reason 弱桩路径 | system_fail 测试构建返回（无存储宿主如实无留痕） |
| 回归 | 架构 47 源恰分类；主机 97 项；CTest 6/6；native_sim 不受影响 |

## 自行决策（建议值，待追认）

① 巡检经测试直调（Linux 宿主无工作线程——调度 no-op 如实登记）；
② clock_id=2（与 Zephyr=1 区分——R01 跨时基不比较语义）；③ 垫片
单头包含（-include）而非改源 include。

## 未测与边界（如实）

ARM64 实跑（当前 x86_64 宿主——D02"按批准环境实跑"待 ARM64 机）；
Wasm 循环在 Linux 宿主（同源 WAMR 面归后续需求时评估）；线程化/
SMP 语义；完整 Linux 伴侣（D02 延期面）。

## 复现

```sh
./build-support/build-linux-host.sh
./build/r07-linux-host/host
```
