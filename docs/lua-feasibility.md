# D03 Lua 真实可行性验证记录（仅 Zephyr）

本记录对应 [包合同](../framework/services/package/contract.md) 的 D03 节。它回答
一个问题：把合同里的内存闸、步数闸和宿主调用接到**真实的 Lua 解释器**上，
行为是否符合合同、代价和边界是什么。它不是 Lua 运行时，不链接进任何产品镜像，
也不代表正式支持已交付（所有者裁决：Lua 只在 Zephyr 中构建，Linux 侧不做）。

所有者裁决（2026-11-24）：

- 依赖以最新版本为准，钉定 **5.5.1**（本文把这句话理解为：5.5.1 即确定采用的
  版本）；嵌套上限保持出厂的 200；沙箱库尽可能开放；已授权并在板上实测。
- **首期不施加步数预算，也不约束 C 代码**。步数预算的机制保留在实验里作为
  证据，启用与否另行裁决。本文把这句话理解为只针对执行时间方面的限制，内存
  配额不在其列。
- **分配改用 PSRAM**，改完重新评估。这一项目前受阻于真板上 PSRAM 不应答，见
  “PSRAM”一节。

本文先前在 5.4.9 上的测量已被 5.5.1 的测量取代，数字以本文为准。

## 范围与对象

- 解释器：`.deps/lua`，钉定提交 `7579fc9`（5.5.1，上游标签 v5.5.1），**不修改
  源码**；构建前由 `tools/check_dependencies.py --dependency lua` 核验。
- 实验应用：[`platform/zephyr/tests/lua_vm`](../platform/zephyr/tests/lua_vm)，
  选择加入（`-DLUA_VM_FEASIBILITY=ON`），不在默认回归集内。它把
  [`lua_feasibility.c`](../framework/services/package/lua_feasibility.c) 的机制桩接到
  三个真实挂点上：内存闸成为 `lua_Allocator` 的记账，步数闸成为 `lua_sethook`
  计数钩子，宿主调用是只信宿主绑定 principal 的 C 闭包（R04 `entelea_perm_check`）。
- 解释器的内存来自一块**专用的内核堆**（静态大小的 `k_heap`），不是 libc 的
  `malloc`（原因见“板上发现”）。
- 实验在自己的线程里运行。启用 PSRAM（`CONFIG_ESP_SPIRAM=y`）时，这个线程的
  栈和解释器堆的缓冲区经链接段放进 PSRAM；否则都是普通的静态对象。
- 向脚本打开 base、coroutine、table、string、math、utf8、io、os、package。
  不打开 `debug`（它能拆掉预算），运行时移除 `os.exit` 和 `os.execute`（它们
  作用于整个固件），并置空 `dofile`/`loadfile`。`debug` 库只为演示风险而编译。

## 复现

```sh
export PATH="$PWD/.venv/bin:$PATH"
ROOT="$PWD"
# 仿真目标（默认配置）
python3 tools/build_zephyr.py --app platform/zephyr/tests/lua_vm --board native_sim \
  --zephyr-base .deps/zephyrproject/zephyr --python .venv/bin/python3 \
  --build-dir build/lua-feas --cmake-arg=-DLUA_VM_FEASIBILITY=ON --build
(cd "$(mktemp -d)" && "$ROOT/build/lua-feas/zephyr/zephyr.exe")
```

测量用参数（均为 CMake 缓存变量，不改钉定源码）：

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `-DLUA_VM_MAXCCALLS=<n>` | 200 | 解释器的 C 调用/语法嵌套上限 `LUAI_MAXCCALLS` |
| `-DLUA_VM_HEAP_KB=<n>` | 320 | 解释器专用内核堆的大小 |
| `-DLUA_VM_QUOTA_KB=<n>` | 256 | 受测的内存配额，不得大于堆 |
| `-DLUA_VM_STACK_KB=<n>` | 1024 | 实验线程的栈；栈测量窗口是它的一半 |

板级（ESP32-P4，`esp32p4c5_core_board/esp32p4/hpcore`）：`tools/build_zephyr.py`
用 `--board-root platform/zephyr`、SDK 1.0.1、`riscv64-zephyr-elf` 14.3.0、模块
`hal_espressif`、`--cmake-arg=-DESP_IDF_PATH=.deps/zephyrproject/modules/hal/espressif`。
真机通过的配置是 `-DLUA_VM_MAXCCALLS=200 -DLUA_VM_QUOTA_KB=128
-DLUA_VM_HEAP_KB=192 -DLUA_VM_STACK_KB=352`。烧录与监听沿用
`factory/forge/gate/gate_run.sh` 的参数：`esptool --chip esp32p4 -p /dev/ttyACM0
-b 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000
build/<dir>/zephyr/zephyr.bin`，控制台 115200，经 EN 引脚硬复位。

## 实测结果

真机：ESP32-P4 修订版 v3.2，主核 400 MHz，16 MB 闪存。仿真是 native_sim（i386，
与目标同为 32 位指针，但线程跑在宿主栈上、libc 是宿主的）。单用例 `lua_feas`
在两者上都通过，失败掩码 0x0。

| 合同条目 | 结果（真机与仿真一致） | 真机测量值 |
| --- | --- | --- |
| 内存预算 | 2 MB 字符串在配额下以 `LUA_ERRMEM` 结束，被**配额闸**拦下，未越界 | 512 KB 配额（合同上限）：拒绝 2 次，已用 524,198 B |
| 数据隔离 | 两个 64 KB 实例互不可见 | 第一个被拒 2 次时，第二个仍能分配 |
| 执行预算与有界停止 | 死循环恰在第 100,000 步以 `LUA_ERRRUN` 停止 | 钩子拒绝 1 次；中止与配额错误之后状态仍可用 |
| 协程内的预算 | 协程里的死循环同样在第 100,000 步停止 | 钩子在创建协程时被继承；恢复者所在脚本一并结束 |
| 宿主调用与可信身份 | 未绑定 principal 为假；绑定后为真 | 脚本传入的实参不参与判定 |
| Lua 间递归 | 由配额结束（`LUA_ERRMEM`），不消耗 C 栈 | 状态可复用 |
| 字符串模式递归 | 在 200 层处报 `pattern too complex` | C 栈 12,259 B |
| 沙箱面 | `io.open` 在板上被拒绝 | 失败即关闭 |

内存（解释器自己的记账）：状态加全部库基线 13,989 B（峰值 14,573 B）；
千元素表脚本峰值 28,767 B。

## 速度

2×10^7 次循环，共 8000 万条 VM 指令：

| 计数钩子触发间隔 | 真机耗时 | 相对无钩子 | 仿真耗时 | 相对无钩子 |
| --- | --- | --- | --- | --- |
| 无钩子 | 5,700 ms | 1× | 108 ms | 1× |
| 每 10000 条指令 | 14,703 ms | 约 2.6× | 334 ms | 约 3.1× |
| 每 100 条指令 | 14,993 ms | 约 2.6× | 347 ms | 约 3.2× |
| 每条指令 | 42,662 ms | 约 7.5× | 963 ms | 约 8.9× |

真机上解释器约每秒 1400 万条 VM 指令。只要设了计数钩子，VM 每条指令都要进
一次跟踪函数（由它决定是否调用钩子），所以调大间隔省不掉基础开销。按此推算，
合同步数上限 10^7 在真机上约 0.7 秒（无钩子）、1.8 秒（每 100 或每万条触发）、
5.4 秒（每条触发）。耗时在重复运行之间有一成左右的波动。

**步数预算只约束 VM 指令，不约束 C 代码。** 宿主函数 `busy(100000000)`（真机
4,000 ms）和 `string.find` 的回挡都跑到底，预算 1000 步时只记到 3 步。回挡每加
一个可选项约翻倍：真机 22 项 22.0 秒，24 项 94.1 秒；几十项就能长时间占住脚本
线程。按裁决，首期不约束这一点。

**`debug` 库能拆掉预算。** `debug.sethook()` 之后 5×10^7 次循环跑完，只记 3 步。

## C 栈开销由嵌套上限决定

到上限时的深度（图样法，是下界；真机每层开销由上限 40 和 80 两点确定，两点
严格线性）：

| `LUAI_MAXCCALLS` | 语法嵌套到上限（真机） | C 调用嵌套到上限（真机） | 语法嵌套（仿真） | C 调用嵌套（仿真） |
| --- | --- | --- | --- | --- |
| 20 | — | — | 4,791 B | 25,879 B |
| 40 | 8,643 B | 58,979 B | 7,991 B | 51,479 B |
| 80 | 15,683 B | 117,875 B | 14,391 B | 102,679 B |
| **200（出厂，已裁决）** | 36,803 B | 294,515 B | 33,591 B | 256,407 B |

真机上语法嵌套约 176 B/层，C 调用嵌套（经 `string.gsub` 回调）约 1,472 B/层。
上限 200 需要约 **294 KB** 的脚本线程栈；这在 PSRAM 上已经精确测到（294,515 B，
栈放在 PSRAM、测量窗口 512 KB），与斜率推算一致。其他路径：空脚本 1,507 B、典型
脚本 1,939 B、钩子中止 2,419 B（真机）。

## 板上发现

**libc 的 `malloc` 用的是镜像剩下的 SRAM，不是 `CONFIG_HEAP_MEM_POOL_SIZE`。**
这块板子上 `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` 取默认的 -1：Zephyr 把 libc
分配器的 arena 定义为 `_end` 到 `_heap_sentry` 之间的剩余 SRAM，而
`CONFIG_HEAP_MEM_POOL_SIZE` 只决定 `k_malloc` 用的内核池。证据是第一次真机
运行：镜像配置了 327,680 B 的内核池，libc arena 实际只有 109,760 B；Lua 在
已用 92,047 B 时因堆耗尽得到 `LUA_ERRMEM`，配额闸的拒绝计数为 0。实验因此改用
专用内核堆。这不只是 Lua 的事：任何经 `malloc`/`realloc` 的分配在这块板上都
只受“镜像剩多少”约束。按裁决改用 PSRAM 后，这一点只对**确实分配在 PSRAM 里**
的内存不再成立：上游没有把 libc `malloc` 接到 PSRAM，要进 PSRAM 必须走 PSRAM
的堆接口（共享多堆 `shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, …)`，或像
本实验这样在 PSRAM 段里建专用 `k_heap`）。libc 的 `malloc` 仍用剩余 SRAM。

**出厂 Lua 在板级镜像上缺符号。** `lauxlib.c` 的 `luaL_loadfilex` 仍引用
`fopen`；io、os 库经 libc 引出 `open/read/write/lseek/close/unlink/rename`；
`os.clock` 需要 `clock()`。板级镜像没有文件系统和进程时钟。实验用
[`no_fs_stubs.c`](../platform/zephyr/tests/lua_vm/src/no_fs_stubs.c) 提供
“失败即关闭”的桩（只在板级编译），真机上 `io.open` 被拒绝。

**数字宽度。** 5.5.1 的 `luaconf.h` 把 `LUA_32BITS` 留作可选定义（5.4.9 是
写死的），所以 32 位整数/浮点现在可以在编译期选择，不必改头文件。本次没有测。
板级目标 rv32imafc 只有单精度 FPU：镜像里没有任何一条双精度硬件指令，
`__adddf3` 等由芯片 ROM 的软件例程提供（地址见厂商的 `esp32p4.rom.rvfp.ld`）。

**小堆实现有上限（仅仿真上观察到）。** native_sim 默认选 `SYS_HEAP_SMALL_ONLY`，
32 位下一个堆超过约 128 KB 就在启动时停机；实验配置改为 `CONFIG_SYS_HEAP_AUTO=y`。

## PSRAM

板子有 32 MB 叠封 PSRAM，DTS 里 `psram0` 已声明，但本工程的 Zephyr 镜像此前从未
启用它。核对源码：所钉定的 Zephyr 4.5 **带有 ESP32-P4 的 PSRAM 支持**
（`CONFIG_ESP_SPIRAM`，Kconfig 默认 HEX 模式、200 MHz；启动时初始化；链接脚本
提供 `.ext_ram_noinit.*` 与 `.ext_ram.bss*` 段和一块共享多堆区域）。板级资源
文档里“esp32p4 无上游 SPIRAM 堆集成”的说法不成立，已更正。

**根因：板级定义漏了 PSRAM 的供电。** 叠封 PSRAM 与它的锁相环由芯片内部的
LDO 通道 2（1.8 V）供电。ESP-IDF 在 `esp_clk_tree_mpll_acquire()` 里打开这个
通道（默认 `ESP_LDO_RESERVE_PSRAM=y`、`ESP_LDO_CHAN_PSRAM_DOMAIN=2`、
`ESP_LDO_VOLTAGE_PSRAM_DOMAIN=1800`）；Zephyr 移植层编译同一份代码时不带这些
选项，改为由**板级设备树**声明该通道为稳压器。上游带叠封 PSRAM 的板子
（waveshare P4 套件、esp32p4x_function_ev_board）都这么写：设备树里
`ldo2@2` 为 1.8 V、`regulator-boot-on`、`regulator-always-on`，默认配置里
`CONFIG_REGULATOR=y`。**本工程的板级定义两者都没有**，于是 PSRAM 与锁相环
根本没上电，驱动的每次读回都是固定的 `0x3b3b3b3b`。

寄存器证据（在 PSRAM 初始化前一刻打印）：

```
diag before: ext_ldo xpd = 1 0 0 0, mspi_phy_xpd=0 mpll500m_en=0
```

2 号通道是关的，PSRAM 的物理层和锁相环也都没上电。打开它之后，同一个驱动立刻
正常：

```
diag after:  ext_ldo xpd = 1 0 0 1, mspi_phy_xpd=0 mpll500m_en=0
D (hex_psram): exp_data: 0x5a6b7c8d      ← 读回与写入一致
I (hex_psram): vendor id : 0x0d (AP)   density : 0x07 (256 Mbit)   BitMode : X16 Mode
```

**修法：走上游的板级做法，本板已验证。** 板级定义里补上设备树的 LDO 声明
（`ldo2@2`：1.8 V、`regulator-boot-on`、`regulator-always-on`；`ldo1@1` 3.3 V
是 ROM 默认就开的闪存通道），默认配置里加 `CONFIG_REGULATOR=y`。这样 Zephyr 的
稳压器驱动在 PRE_KERNEL_1、优先级 20 先打开 2 号通道，内核在优先级 40
（`soc/espressif/common/esp_psram.c` 的 `esp32p4_psram_init`）初始化 PSRAM。
hal_espressif 侧也是这个约定：`esp_clk_tree_common.c` 在 `CONFIG_REGULATOR_ESP32`
存在时跳过自己的 LDO 获取，避免双重使能。

此前“稳压器驱动那条路会带进 PMU 时钟驱动、板子起不来”的判断**不成立，已撤回**：
PMU 时钟驱动（`drivers/clock_control/clock_control_esp32_pmu.c`）对所有 ESP32-P4
镜像都编译进去，与 `CONFIG_REGULATOR` 无关——应用侧供电版与稳压器驱动版两个镜像
的 map 里它都在。

真机验证（2026-11-24，镜像 `build/board-lua-psram-regulator`，只有板级定义与稳
压器驱动，没有任何应用侧供电代码）：

```
I (hex_psram): vendor id : 0x0d (AP)  density : 0x07 (256 Mbit)  BitMode : X16 Mode
[lua-feas] layout: PSRAM initialized 1, reported size 33554432 bytes
[lua-feas] layout: harness thread stack 0x48000000 (1048576 bytes), interpreter heap 0x48100280 (1048576 bytes), in PSRAM: 1
[lua-feas] failed mask=0x0
PROJECT EXECUTION SUCCESSFUL
```

213.5 秒，与此前应用侧供电的结果一致。应用侧的临时文件
`platform/zephyr/tests/lua_vm/src/psram_power.c` 已删除。

**与 ESP-IDF 6.1 的对照（同一块板）。** 按所有者要求在 `~/sdk/esp-idf-v6.1`
安装了 ESP-IDF 6.1，并做了最小自检（`~/workspace/sandbox/esp-idf-psram-selftest`）：

```
I (hex_psram): vendor id : 0x0d (AP)   density : 0x07 (256 Mbit)   BitMode : X16 Mode
I (269) esp_psram: Found 32MB PSRAM device
I (269) esp_psram: Speed: 200MHz
I (1151) esp_psram: SPI SRAM memory test OK

psram initialized by the driver: 1
psram size reported: 33554432 bytes
allocated 31 blocks of 1 MB
pattern check over 31 MB: 0 wrong words
sequential write: 83.7 MB/s over 31 MB
sequential read: 106.5 MB/s over 31 MB
RESULT: PASS
```

ESP-IDF 下的 PSRAM 完全正常（32 MB 逐字写入读回零错误），说明硬件没有问题；
Zephyr 侧缺的就是那一步供电。IDF 的驱动文件与本工程钉定的那份**逐行相同**
（`esp_psram_impl_ap_hex.c`，605 行，只差一行日志级别），差别只在系统层与
板级配置。

**此前那些排除性结论，现在可以说明原因了**（都发生在 PSRAM 无电的状态下）：
20/80/200 MHz、ECC 开关、闪存 80/40 MHz 六种配置同样失败，因为没有电；
上游 2026-09-28 的 ECC 残留修复不适用（相关寄存器本来就是 0）；片选脉冲会改变
现象但不能救活无电的芯片。**“需要断电重启”这一说法是错的**：ESP-IDF 成功初始化
PSRAM 之后、同一块板没有断过电，紧接着刷入 Zephyr 镜像，读回的仍是
`0x3b3b3b3b`；而只补上供电这一步、其余不变，Zephyr 立刻通过。

**PSRAM 上的最终结果（Zephyr，真机）：**

| 项 | 值 |
| --- | --- |
| PSRAM 识别 | vendor 0x0d（AP），density 0x07（256 Mbit = 32 MB），X16 模式，200 MHz |
| 合同检查 | 失败掩码 0x0，全部通过 |
| 内存配额 | 512 KB（合同上限）下 2 MB 申请被配额拦下，拒绝 2 次，已用 524,198 B |
| 嵌套 C 调用到上限 200 的 C 栈深度 | **294,515 B**（精确值，窗口 512 KB 足够） |
| 语法嵌套到上限 | 36,803 B |
| 静态 HP SRAM | 90,380 / 786,432 B |
| 静态 PSRAM | 3,146,368 / 33,554,432 B（含上游预留的 1 MB 共享堆） |
| 总内存需求 | 3,236,748 / 34,340,864 B → 三档全部满足 |

数据放在 PSRAM 的速度代价很小：解释器循环 5.95 秒（同样的测试在 SRAM 上是 5.70
秒，约慢 4%），回挡测试 23.6 秒对 22.0 秒（约慢 7%）。

## 体积（ESP32-P4 交叉编译，-Os，5.5.1 全库）

| 项 | 值 |
| --- | --- |
| Lua 归档（代码+只读数据，含 `debug` 库） | 149,912 B；段回收丢弃 1,332 B，镜像内保留 **148,580 B** |
| Lua 静态 RAM | 0（解释器堆与线程栈是另外的静态分配） |
| 随之链入的 libm | 17 个函数，15,648 B |
| 随之链入的 picolibc 双精度转换 | `strtod`、`__atod_engine`、`__dtoa_engine` 2,672 B |
| 真机通过的测试镜像 | 代码 274,692 B；`zephyr.bin` 376,704 B |

16 MB 闪存下 Lua 约占 1%，不是约束。约束在栈和堆：栈与堆都在 SRAM 的真机镜像
静态 HP SRAM 为 638,448 B，**超出 552,960 B 的 D12 红线**；这是量测镜像，不是
产品镜像。现有产品镜像最多已用 481,648 B，按裁决的嵌套上限 200，再加一个约
300 KB 的脚本线程是放不下的。这也是要改用 PSRAM 的原因。

## 没有测的

- PSRAM 上运行的速度：已测（解释器循环慢约 4%、回挡慢约 7%），但只有 Lua 这一个
  负载；网络、TLS、flash 写入等负载没有在 PSRAM 上测过。
- 32 位数字配置的体积与速度（现在可用 `-DLUA_32BITS` 选择）。
- 配额下限：合同域是 1..512 KB，而状态加全部库基线已是 14 KB（推断几 KB 的
  配额打不开状态，未实测）。512 KB 的配额在 SRAM 上放不下，在 PSRAM 上还没
  能验证。
- 上限 200 时 C 调用嵌套在真机上的精确深度：已测（294,515 B）。
- 最坏情况的 C 栈：只测了所列路径，不排除其他库函数每层更重。
- 脚本线程与其他线程的调度关系、长时间运行、其他板子。

## 测量方法上需要知道的

- 仿真器（Zephyr 4.5.0）的线程跑在宿主 pthread 栈上，内核的栈水位 API 在这里
  报的是 Zephyr 栈缓冲里的线程状态结构体大小（24 字节），不是栈用量。实验改为
  在调用前把栈指针以下的窗口写成图样、事后找最深的被改写字节。函数帧里从未
  被写到的字节会保留图样，所以结果是下界；窗口是实验线程栈的一半，深度等于
  窗口即“至少”。
- 仿真时钟在代码运行时不推进，实验在仿真目标上直接读宿主的单调时钟；真机上
  读 `k_uptime_get()`，分辨率 1 ms。
- 仿真的 io 与 os 确实能访问宿主文件系统，所以“打开文件被拒绝”只在板上断言。

## 真机过程

烧录前把 16 MB 闪存完整备份到仓库之外（`/home/emb/project/entelea-board-backup/`，
含校验和）；板上原先是一个 ESP-IDF v6.1 应用，已被测试镜像覆盖。SRAM 上共三次
完整运行通过（上限 80、上限 200，以及改造为独立线程后的上限 200），每次约
215–225 秒；PSRAM 侧先失败六次（原因见“PSRAM”一节），补上供电后三次通过：一次是
带临时诊断代码的镜像（228 秒），一次是应用侧供电的正式版本（228 秒，失败掩码
0x0），最后一次是改用板级定义供电的版本（213.5 秒，失败掩码 0x0）。
板上现在驻留的是最后那个 PSRAM 镜像，每次复位运行约 4 分钟。

## 需要所有者裁决

已裁决：依赖为最新版 5.5.1；嵌套上限保持 200；库尽可能开放；上板测试；首期
不施加步数预算、不约束 C 代码；分配改用 PSRAM；内存红线为三档、按总内存
（最低 M − D ≥ 64 KB、紧凑 D ≤ 0.70 M、宽松 D ≤ 0.50 M）。

仍待裁决或需要所有者动手：

1. **PSRAM 的用量红线**：先前的裁决是“启用即新批、要定新红线”。栈和堆放进
   PSRAM 后 SRAM 占用降到 90 KB，PSRAM 用量规则还没有定。
2. **脚本线程的栈**：在 PSRAM 可用之前，按上限 200 需要约 300 KB 的栈，产品
   镜像放不下；是否在 PSRAM 可用前临时降低上限。
3. 文件加载函数（`dofile`/`loadfile`）保持关闭是否同意：它们没有文件可加载，
   将来有文件系统时也应经过宿主的权限闸。
5. 数字类型：保持 `double`（板上走 ROM 软浮点）还是改 32 位。
6. 5.5.1 是否批准为产品镜像的构建依赖（本次裁决确定了版本）。
7. 板上现在驻留的是测试镜像，每次复位运行约 4 分钟；原固件可随时从备份恢复。
