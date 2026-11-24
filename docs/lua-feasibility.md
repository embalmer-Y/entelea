# D03 Lua 真实可行性验证记录（仅 Zephyr）

本记录对应 [包合同](../framework/services/package/contract.md) 的 D03 节。它回答
一个问题：把合同里的内存闸、步数闸和宿主调用接到**真实的 Lua 解释器**上，
行为是否符合合同、代价和边界是什么。它不是 Lua 运行时，不链接进任何产品镜像，
也不代表正式支持已交付（所有者裁决：Lua 只在 Zephyr 中构建，Linux 侧不做）。

所有者裁决（2026-11-24）：依赖以最新版本为准，钉定 **5.5.1**；嵌套上限保持
出厂的 200；沙箱库尽可能开放；已授权并在板上实测。“以最新版本为准”本文理解为：5.5.1 即确定采用的依赖版本。
本文先前在 5.4.9 上的测量已被 5.5.1 的测量取代，结论方向相同，数字以本文为准。

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
- 向脚本打开 base、coroutine、table、string、math、utf8、io、os、package。
  不打开 `debug`（它能拆掉预算），运行时移除 `os.exit` 和 `os.execute`（它们
  作用于整个固件），并置空 `dofile`/`loadfile`。`debug` 库只为演示风险而编译。

## 复现

```sh
export PATH="$PWD/.venv/bin:$PATH"
ROOT="$PWD"
# 仿真目标（默认配置；栈测量窗口为线程栈的一半）
python3 tools/build_zephyr.py --app platform/zephyr/tests/lua_vm --board native_sim \
  --zephyr-base .deps/zephyrproject/zephyr --python .venv/bin/python3 \
  --build-dir build/lua-feas --cmake-arg=-DLUA_VM_FEASIBILITY=ON --build
(cd "$(mktemp -d)" && "$ROOT/build/lua-feas/zephyr/zephyr.exe")
```

测量用参数（均为 CMake 缓存变量，不改钉定源码）：

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `-DLUA_VM_MAXCCALLS=<n>` | 200 | 解释器的 C 调用/语法嵌套上限 `LUAI_MAXCCALLS` |
| `-DLUA_VM_HEAP_KB=<n>` | 320 | 解释器专用内核堆的大小（静态 SRAM） |
| `-DLUA_VM_QUOTA_KB=<n>` | 256 | 受测的内存配额，不得大于堆 |
| `-DCONFIG_ZTEST_STACK_SIZE=<n>` | 65536 | 测试线程栈；栈测量窗口是它的一半 |

板级（ESP32-P4，`esp32p4c5_core_board/esp32p4/hpcore`）：`tools/build_zephyr.py`
用 `--board-root platform/zephyr`、SDK 1.0.1、`riscv64-zephyr-elf` 14.3.0、模块
`hal_espressif`、`--cmake-arg=-DESP_IDF_PATH=.deps/zephyrproject/modules/hal/espressif`。
真机通过的配置是 `-DLUA_VM_MAXCCALLS=200 -DLUA_VM_QUOTA_KB=128
-DLUA_VM_HEAP_KB=192 -DCONFIG_ZTEST_STACK_SIZE=327680`。烧录与监听沿用
`factory/forge/gate/gate_run.sh` 的参数：`esptool --chip esp32p4 -p /dev/ttyACM0
-b 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000
build/<dir>/zephyr/zephyr.bin`，控制台 115200，经 EN 引脚硬复位。

## 实测结果

真机：ESP32-P4 修订版 v3.2，主核 400 MHz，16 MB 闪存。仿真是 native_sim（i386，
与目标同为 32 位指针，但线程跑在宿主栈上、libc 是宿主的）。单用例 `lua_feas`
在两者上都通过，失败掩码 0x0。

| 合同条目 | 结果（真机与仿真一致） | 真机测量值 |
| --- | --- | --- |
| 内存预算 | 2 MB 字符串在配额下以 `LUA_ERRMEM` 结束，被**配额闸**拦下，未越界 | 128 KB 配额：拒绝 2 次，已用 131,009 B |
| 数据隔离 | 两个 64 KB 实例互不可见 | 第一个被拒 2 次时，第二个仍能分配 |
| 执行预算与有界停止 | 死循环恰在第 100,000 步以 `LUA_ERRRUN` 停止 | 钩子拒绝 1 次；中止与配额错误之后状态仍可用 |
| 协程内的预算 | 协程里的死循环同样在第 100,000 步停止 | 钩子在创建协程时被继承；恢复者所在脚本一并结束 |
| 宿主调用与可信身份 | 未绑定 principal 为假；绑定后为真 | 脚本传入的实参不参与判定 |
| Lua 间递归 | 由配额结束（`LUA_ERRMEM`），不消耗 C 栈 | 状态可复用 |
| 字符串模式递归 | 在 200 层处报 `pattern too complex` | C 栈 12,259 B |
| 沙箱面 | `io.open` 在板上被拒绝 | 失败即关闭 |

内存（解释器自己的记账）：状态加全部库基线 13,989 B（峰值 14,573 B）；
千元素表脚本峰值 28,767 B。

## 速度与代价

2×10^7 次循环，共 8000 万条 VM 指令：

| 计数钩子触发间隔 | 真机耗时 | 相对无钩子 | 仿真耗时 | 相对无钩子 |
| --- | --- | --- | --- | --- |
| 无钩子 | 5,700 ms | 1× | 103 ms | 1× |
| 每 10000 条指令 | 14,703 ms | 约 2.6× | 310 ms | 约 3.0× |
| 每 100 条指令 | 15,004 ms | 约 2.6× | 327 ms | 约 3.2× |
| 每条指令 | 43,933 ms | 约 7.7× | 936 ms | 约 9.1× |

真机上解释器约每秒 1400 万条 VM 指令。只要设了计数钩子，VM 每条指令都要进
一次跟踪函数（由它决定是否调用钩子），所以调大间隔省不掉基础开销。按此推算，
合同步数上限 10^7 在真机上约 0.7 秒（无钩子）、1.8 秒（每 100 或每万条触发）、
5.5 秒（每条触发）。耗时在重复运行之间有一成左右的波动。

**步数闸只约束 VM 指令，不约束 C 代码。** 宿主函数 `busy(100000000)`（真机
4,000 ms）和 `string.find` 的回挡都跑到底，预算 1000 步时只记到 3 步。回挡每加
一个可选项约翻倍：真机 22 项 22.0 秒，24 项 94.1 秒；几十项就能长时间占住脚本
线程。

**`debug` 库能拆掉预算。** `debug.sethook()` 之后 5×10^7 次循环跑完，只记 3 步。

## C 栈开销由嵌套上限决定

到上限时的深度（图样法，是下界；真机每层开销由上限 40 和 80 两点确定，两点
严格线性）：

| `LUAI_MAXCCALLS` | 语法嵌套到上限（真机） | C 调用嵌套到上限（真机） | 语法嵌套（仿真） | C 调用嵌套（仿真） |
| --- | --- | --- | --- | --- |
| 20 | — | — | 4,791 B | 25,879 B |
| 40 | 8,643 B | 58,979 B | 7,991 B | 51,479 B |
| 80 | 15,683 B | 117,875 B | 14,391 B | 102,679 B |
| **200（出厂，已裁决）** | 36,803 B | ≥163,840 B（实测在 320 KB 栈内完成） | 33,591 B | 256,279 B |
| 400 | — | — | 65,591 B | 512,279 B |

真机上语法嵌套约 176 B/层，C 调用嵌套（经 `string.gsub` 回调）约 1,472 B/层。
按斜率，上限 200 需要约 **294 KB** 的脚本线程栈，上限 400 需要约 589 KB。
其他路径：空脚本 1,507 B、典型脚本 1,939 B、钩子中止 2,419 B（真机）。

## 板上发现

**libc 的 `malloc` 用的是镜像剩下的 SRAM，不是 `CONFIG_HEAP_MEM_POOL_SIZE`。**
这块板子上 `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` 取默认的 -1：Zephyr 把 libc
分配器的 arena 定义为 `_end` 到 `_heap_sentry` 之间的剩余 SRAM，而
`CONFIG_HEAP_MEM_POOL_SIZE` 只决定 `k_malloc` 用的内核池。证据是第一次真机
运行：镜像配置了 327,680 B 的内核池，libc arena 实际只有 109,760 B；Lua 在
已用 92,047 B 时因堆耗尽得到 `LUA_ERRMEM`，配额闸的拒绝计数为 0。实验因此改用
专用内核堆。这不只是 Lua 的事：任何经 `malloc`/`realloc` 的分配在这块板上都
只受“镜像剩多少”约束。

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

## 体积（ESP32-P4 交叉编译，-Os，5.5.1 全库）

| 项 | 值 |
| --- | --- |
| Lua 归档（代码+只读数据，含 `debug` 库） | 149,912 B；段回收丢弃 1,332 B，镜像内保留 **148,580 B** |
| Lua 静态 RAM | 0（解释器堆与线程栈是另外的静态分配） |
| 随之链入的 libm | 17 个函数，15,648 B |
| 随之链入的 picolibc 双精度转换 | `strtod`、`__atod_engine`、`__dtoa_engine` 2,672 B |
| 整个测试镜像 | 代码 274,300 B；`zephyr.bin` 376,448 B |

16 MB 闪存下 Lua 约占 1%，不是约束。约束在栈和堆：真机通过的镜像静态 HP SRAM
约 604 KB（脚本线程栈 320 KB + 解释器堆 192 KB + 测试与内核自身），**超出
552,960 B 的 D12 红线**；这是量测镜像，不是产品镜像。现有产品镜像最多已用
481,648 B，按裁决的嵌套上限 200，再加一个约 300 KB 的脚本线程是放不下的。

## 没有测的

- 32 位数字配置的体积与速度（现在可用 `-DLUA_32BITS` 选择）。
- 配额下限：合同域是 1..512 KB，而状态加全部库基线已是 14 KB（推断几 KB 的
  配额打不开状态，未实测）。512 KB 配额需要的堆在这块板子上放不下。
- 上限 200 时 C 调用嵌套的精确深度：真机测量窗口只到 160 KB，294 KB 是由线性
  斜率推算的。
- 最坏情况的 C 栈：只测了所列路径，不排除其他库函数每层更重。
- 脚本线程与其他线程的调度关系、长时间运行、其他板子。

## 测量方法上需要知道的

- 仿真器（Zephyr 4.5.0）的线程跑在宿主 pthread 栈上，内核的栈水位 API 在这里
  报的是 Zephyr 栈缓冲里的线程状态结构体大小（24 字节），不是栈用量。实验改为
  在调用前把栈指针以下的窗口写成图样、事后找最深的被改写字节。函数帧里从未
  被写到的字节会保留图样，所以结果是下界；窗口是线程栈的一半，深度等于窗口
  即“至少”。
- 仿真时钟在代码运行时不推进，实验在仿真目标上直接读宿主的单调时钟；真机上
  读 `k_uptime_get()`，分辨率 1 ms。
- 仿真的 io 与 os 确实能访问宿主文件系统，所以“打开文件被拒绝”只在板上断言。

## 真机过程

烧录前把 16 MB 闪存完整备份到仓库之外（`/home/emb/project/entelea-board-backup/`，
含校验和）；板上原先是一个 ESP-IDF v6.1 应用，已被测试镜像覆盖。共烧录运行
三次：第一次（libc 堆）因上述堆问题配额检查失败，失败掩码 0x2；第二次
（上限 80）与第三次（上限 200）全部通过。每次运行约 215–225 秒。

## 需要所有者裁决

已裁决：依赖为最新版 5.5.1；嵌套上限保持 200；库尽可能开放；上板测试。

仍待裁决：

1. **脚本线程的栈**：按上限 200 需要约 300 KB，产品镜像放不下。降低上限、
   接受这个开销，还是另作安排。
2. **步数闸方案**：是否接受计数钩子约 2.6 倍的吞吐代价并选定触发间隔，还是
   另找有界停止机制。
3. **C 代码不受步数约束**怎么处置：限制宿主函数的执行时间；是否向沙箱开放
   `string.find`、`gsub` 这类会回挡的函数。
4. 解释器使用专用内核堆（本实验的做法）是否作为接入设计定下来。
5. libc `malloc` 只受剩余 SRAM 约束这条板级事实，是否需要在框架层面处置
   （例如 D12 的堆余量针对哪一个堆）。
6. 文件加载函数（`dofile`/`loadfile`）保持关闭是否同意：它们没有文件可加载，
   将来有文件系统时也应经过宿主的权限闸。
7. 数字类型：保持 `double`（板上走 ROM 软浮点）还是改 32 位。
8. 板上现在驻留的是测试镜像，每次复位运行约 4 分钟；原固件可随时从备份恢复。
