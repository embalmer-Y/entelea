# D03 Lua 真实可行性验证记录（仅 Zephyr）

本记录对应 [包合同](../framework/services/package/contract.md) 的 D03 节。它回答
一个问题：把合同里的内存闸、步数闸和宿主调用接到**真实的 Lua 5.4.9 解释器**上，
行为是否符合合同、代价和边界是什么。它不是 Lua 运行时，不链接进任何产品镜像，
也不代表正式支持已交付（所有者裁决：Lua 只在 Zephyr 中构建，Linux 侧不做）。

## 范围与对象

- 解释器：`.deps/lua`，钉定提交 `312b9ef`（5.4.9），**不修改源码**；构建前由
  `tools/check_dependencies.py --dependency lua` 核验。
- 实验应用：[`platform/zephyr/tests/lua_vm`](../platform/zephyr/tests/lua_vm)，
  选择加入（`-DLUA_VM_FEASIBILITY=ON`），不在默认回归集内。它把
  [`lua_feasibility.c`](../framework/services/package/lua_feasibility.c) 的机制桩接到
  三个真实挂点上：内存闸成为 `lua_Allocator` 的记账，步数闸成为 `lua_sethook`
  计数钩子，宿主调用是只信宿主绑定 principal 的 C 闭包（R04 `entelea_perm_check`）。
- 解释器只打开 base、coroutine、table、string、math、utf8；不编译 io、os、
  package 与 `linit`；运行时把 `dofile`/`loadfile` 置空。`debug` 库只为演示风险
  而编译。

## 复现

```sh
export PATH="$PWD/.venv/bin:$PATH"
# 仿真目标（默认配置；栈测量窗口为线程栈的一半）
python3 tools/build_zephyr.py --app platform/zephyr/tests/lua_vm --board native_sim \
  --zephyr-base .deps/zephyrproject/zephyr --python .venv/bin/python3 \
  --build-dir build/lua-feas --cmake-arg=-DLUA_VM_FEASIBILITY=ON --build
ROOT="$PWD"
(cd "$(mktemp -d)" && "$ROOT/build/lua-feas/zephyr/zephyr.exe")
```

测量用参数（均为 CMake 缓存变量，不改钉定源码）：

| 参数 | 含义 |
| --- | --- |
| `-DCONFIG_ZTEST_STACK_SIZE=2097152` | 线程栈 2 MB，栈测量窗口 1 MB，可分辨出厂设置下的深度 |
| `-DLUA_VM_MAXCCALLS=<n>` | 解释器的 C 调用/语法嵌套上限 `LUAI_MAXCCALLS`（出厂 200） |

板级只做了交叉编译（未上板）：在 `tools/build_zephyr.py` 上用
`--board esp32p4c5_core_board/esp32p4/hpcore --board-root platform/zephyr`，
SDK 1.0.1、`riscv64-zephyr-elf` 14.3.0、模块 `hal_espressif`，
`--cmake-arg=-DESP_IDF_PATH=.deps/zephyrproject/modules/hal/espressif`，
并加 `-DLUA_VM_FEASIBILITY=ON`。

## 实测结果（native_sim，i386，与目标同为 32 位指针）

单用例 `lua_feas` 通过，失败掩码 0x0；下表是同一次运行的测量值。

| 合同条目 | 结果 | 测量值 |
| --- | --- | --- |
| 内存预算 | 2 MB 字符串在 256 KB 配额下以 `LUA_ERRMEM` 结束，未越过配额 | 状态+库基线 9,550 B；千元素表脚本峰值 22,925 B；拒绝 2 次时已用 262,022 B |
| 数据隔离 | 两个 64 KB 实例互不可见 | 第一个被拒 2 次时，第二个仍能分配 |
| 执行预算与有界停止 | 死循环恰在第 100,000 步以 `LUA_ERRRUN` 停止 | 钩子拒绝 1 次；中止与配额错误之后状态仍可用 |
| 协程内的预算 | 协程里的死循环同样在第 100,000 步停止 | 钩子在创建协程时被继承；停止是彻底的，恢复者所在脚本一并结束 |
| 宿主调用与可信身份 | 未绑定 principal 为假；绑定后为真 | 脚本传入的实参不参与判定 |
| Lua 间递归 | 由配额结束（`LUA_ERRMEM`），不消耗 C 栈 | 状态可复用 |
| 字符串模式递归 | 在 200 层处报 `pattern too complex` | C 栈 13,007 B |

## 发现的边界

以下每一条都已实测，不是推断；正式接入前必须在合同或宿主设计中处置。

**步数闸只约束 VM 指令，不约束 C 代码。** 宿主函数 `busy(100000000)`（约 69 ms）
和 `string.find` 的回挡都跑到底，预算 1000 步时只记到 3 步。回挡每加一个可选项
约翻倍：22 项 673 ms，24 项 2,920 ms。板上更慢，几十项就能长时间占住脚本线程。

**`debug` 库能拆掉预算。** `debug.sethook()` 之后 5×10^7 次循环跑完，只记 3 步。
沙箱不能打开 `debug`。

**计数钩子的吞吐代价很高。** 2×10^7 次循环（8000 万条 VM 指令）：

| 钩子触发间隔 | 耗时 | 相对无钩子 |
| --- | --- | --- |
| 无钩子 | 113 ms | 1× |
| 每 10000 条指令 | 325 ms | 约 2.9× |
| 每 100 条指令 | 325 ms | 约 2.9× |
| 每条指令 | 938 ms | 约 8.3× |

重复运行之间耗时有一成左右的变化：另一次默认配置运行的比例是
2.6×、2.7×、7.8×。

只要设了计数钩子，5.4.9 的 VM 每条指令都进一次跟踪函数（由它决定是否调用
钩子），所以调大间隔省不掉基础开销。

**C 栈开销由 `LUAI_MAXCCALLS` 决定，且远大于直觉。** 到达上限时的深度（图样法，
是下界）：

| `LUAI_MAXCCALLS` | 语法嵌套到上限 | 经 `string.gsub` 回调的 C 调用嵌套到上限 |
| --- | --- | --- |
| 20 | 4,455 B | 25,895 B |
| 40 | 7,655 B | 51,815 B |
| 80 | 14,055 B | 103,655 B |
| **200（出厂）** | 33,255 B | **259,175 B** |
| 400 | 65,255 B | 518,375 B |

语法嵌套约 160 B/层，C 调用嵌套约 1.3 KB/层，两者都是线性。空脚本 1,591 B、
典型脚本 2,175 B、钩子中止 1,935 B。也就是说：按出厂设置，“C 栈溢出”这个保护
只有在脚本线程有约 260 KB 栈时才真正成立；要在 32 KB 的栈上成立，上限须压到 24
左右。上限是编译期选项（`llimits.h` 带 `#if !defined` 保护），不需要改源码。

**板级镜像缺符号，出厂 Lua 链接不过。** `lauxlib.c` 的 `luaL_loadfilex` 即使在
运行时被置空，仍引用 `fopen`，经 libc 引出 `open/read/write/lseek/close`；
`ltablib.c` 用 `clock()` 和 `time()` 随机化排序枢轴。板级镜像没有文件系统和进程
时钟。实验用 [`no_fs_stubs.c`](../platform/zephyr/tests/lua_vm/src/no_fs_stubs.c)
提供“失败即关闭”的桩（只在板级编译；仿真镜像用宿主 libc）。副作用：没有时钟时
`table.sort` 的枢轴是可预测的。

**数字宽度改不了命令行选项。** `luaconf.h` 把 `LUA_32BITS` 写死为 0，整数与浮点
类型也无条件定义。板级目标 rv32imafc 只有单精度 FPU：镜像里没有任何一条双精度
硬件指令，`__adddf3` 等由芯片 ROM 的软件例程提供（地址见厂商的
`esp32p4.rom.rvfp.ld`）。要用 32 位整数/浮点只能改头文件。

**内核堆有上限（仅仿真上观察到）。** `CONFIG_SYS_HEAP_SMALL_ONLY=y` 时，
`CONFIG_HEAP_MEM_POOL_SIZE=2097152` 让仿真器启动即停机；实验改用 96 KB。

## 板级交叉编译（未运行）

| 项 | 值 |
| --- | --- |
| Lua 归档（代码+只读数据，含 `debug` 库） | 129,091 B；段回收丢弃 2,290 B，镜像内保留 **126,801 B** |
| Lua 静态 RAM | 0 |
| 随之链入的 libm | 17 个函数，15,644 B（`sin`/`cos`/`exp`/`pow` 等） |
| 随之链入的 picolibc 双精度转换 | `strtod`、`__atod_engine`、`__dtoa_engine` 2,666 B（另有 ryu 表未归因） |
| 整个测试镜像 | 代码 241,144 B；静态 HP SRAM 238,136 / 552,960 B，`check_board_ram.py` 通过 |
| 镜像里属于实验自身的 RAM | 测试线程栈 64 KB + 内核堆 96 KB |

16 MB 闪存下 Lua 约占 1%，不是约束。约束在栈、堆和速度。

## 没有测的

- **板上一切运行结果**：RISC-V 的帧大小（上表的栈深度是 i386 的，不能直接当板
  子的数）、真实 picolibc 堆的行为、钩子和软浮点的实际耗时。上板需要硬件授权。
- **32 位数字配置**的体积与速度：需要派生的 `luaconf.h`，未做。
- **配额下限**：合同域是 1..512 KB，而状态+库基线已是 9.5 KB（推断 1 KB 打不开
  状态，未实测）。512 KB 配额需要的后备堆在板上是否可行也未测。
- **步数预算上限 10^7 的墙钟代价**（合同域）：仿真上无钩子约 15 ms、每条指令
  触发约 117 ms，板上未知。
- 最坏情况的 C 栈：上表是所测路径上的最深值，不排除其他库函数每层更重。

## 测量方法上需要知道的

- 仿真器（Zephyr 4.5.0）的线程跑在宿主 pthread 栈上，内核的栈水位 API 在这里
  报的是 Zephyr 栈缓冲里的线程状态结构体大小（24 字节），不是栈用量。实验改为
  在调用前把栈指针以下的窗口写成图样、事后找最深的被改写字节。函数帧里从未
  被写到的字节会保留图样，所以结果是下界；默认窗口 32 KB 会把出厂上限下的
  C 调用嵌套截成“至少 32768”。
- 仿真时钟在代码运行时不推进（周期计数与 `k_uptime_get()` 都给出 0），实验在
  仿真目标上直接读宿主的单调时钟。
- 仿真镜像的 libc 是宿主的，`realloc` 不经过内核堆；板上才是 picolibc + 内核堆。

## 需要所有者裁决

1. 是否批准钉定的 Lua 5.4.9 作为产品镜像的构建依赖（现为“未批准新依赖”）。
2. `LUAI_MAXCCALLS` 与脚本线程栈大小一起定：见上表，二者乘性相关。
3. 步数闸方案：接受计数钩子 ≥2.6× 的吞吐代价并选定触发间隔，还是另找有界停止
   机制（改 Lua 源码或其他设计）。
4. C 代码不受步数约束怎么处置：限制宿主函数的执行时间；是否向沙箱开放带模式的
   `string.find`/`gsub` 等回挡函数。
5. 沙箱库集合与文件加载器的处置方式（保持置空 + 桩，或自定义 auxlib）。
6. 数字类型：保持 `double`（板上走 ROM 软浮点）还是改 32 位；这需要上板测速。
7. 是否授权上板测量（栈、堆、耗时），以及合同的配额/步数域是否据此调整。
