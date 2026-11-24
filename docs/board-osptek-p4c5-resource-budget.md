# Osptek ESP32-P4C5 核心板 · 资源预算与内存实测表（board 专属）

> 本表为 D12 产品阈值裁决的**实测数据底座**（owner 2026-11-10 指示建立）。
> 纪律：每个新板级 app 随批补行；数据=构建产物实测（elf size/map），
> 不写计划数。构建命令同批各 validation 文档复现节。
> 表格基线：r15 批（2026-11-10），工具链 riscv64-zephyr-elf 14.3.0
> / Zephyr 4.5 pin 41d10b59 / hal_espressif cc81db00。

## 1. 物理内存面（SoC/板）

| 资源 | 容量 | 说明 |
|---|---|---|
| HP SRAM（sramhp@0x4ff00000） | 768 KB | 主运行 RAM（dram0 段+noinit 从中划） |
| 其中 nocache 窗（sramhp_nocache） | 16 KB | DMA 一致性窗口（PMA 非 cache 映射） |
| LP SRAM | 32 KB | LP 域；仅 guard 的复位风暴计数使用 retained RAM（`guardretained`），其余未用 |
| PSRAM（psram0，叠封） | 32 MB | 2026-11-24 已启用并实测可用。所钉定的 Zephyr 4.5 带有 ESP32-P4 的 PSRAM 支持（`CONFIG_ESP_SPIRAM`），此前“无上游集成”的说法不成立。**供电**：叠封 PSRAM 与其锁相环由芯片内部 LDO 通道 2（1.8 V）供电，ESP-IDF 默认打开它，Zephyr 侧要求板级设备树声明该通道——本板原定义漏了这项，导致 PSRAM 无电；现已按上游做法补上（设备树 `ldo2@2` + `CONFIG_REGULATOR=y`）并真机验证。见 [lua-feasibility.md](lua-feasibility.md) |
| NOR flash（boya） | 16 MB | 分区表 0x2000 布局（storage 1MB@0x120000） |
| RTC VBAT | — | 88-pin pad 84（严禁悬空，接 3.3V） |

## 2. 固件占用实测（riscv64-zephyr-elf-size，text=flash 代码；total_ram=data+bss）

| 镜像（build dir） | 用途 | text | total_ram | 关键段备注 |
|---|---|---|---|---|
| r14-minimal | 安全常驻监督 | 69 KB | 699 KB | bss 含全量 RAM 镜像数组（未裁 NO_RAM_BACKEND 的历史面） |
| r02-hwgate | WDT 硬件闸 | 72 KB | 697 KB | 同上 |
| r14-lvlprobe | 电平测量探针 | 70 KB | 699 KB | 同上；该应用已删除（d7a8e1b） |
| r14-tf-final | TF 对拍探针 | 77 KB | 134 KB | NO_RAM_BACKEND=1（对照行：裁剪后 -565 KB；为该次构建的变体，现存 `tf_probe` 默认不定义，数值见“RF1 静态红线复核”） |
| r03-hwflash | flash 持久闸 | 83 KB | 150 KB | NO_RAM_BACKEND=1 |
| f1-wifi-gate | WiFi/netmon 闸 | 231 KB | 511 KB | esp_hosted+网络栈 |
| d3-gate-board | 工厂闭环（WAMR+验签） | 173 KB | 543 KB | WAMR RISCV32_ILP32F |
| **r15-cloud** | **MCU 真云（TLS1.3）** | **505 KB** | **884 KB（size 计）** | **实测落位 ≈403KB SRAM**：.dram0.noinit 271KB（esp_hosted SDIO DMA 池/TLS 静态缓冲）+.dram0.bss 133KB；size 的 bss 计数含 noinit，非全部占初始化预算——**跑通实证预算内** |

### RF1 静态红线复核

旧 `check_board_ram.py` 混计嵌套输入段、漏计部分输出段，且会对空
map 报通过；历史表格及 r16 的旧脚本结果不能当作精确红线证据。
修正后按 GNU ld 的 HP SRAM **输出段**求和（含指令、data、bss、
noinit 和内核表）。脚本拒绝缺少必要地标、穿越窗口边界或重叠的 HP
段，但仅凭文本地标不能证明任意 map 未被删改；以下本轮交叉构建产物
与对应目标 ELF 的 `size -A` 逐镜像对拍一致：

| 镜像 | 输出段合计（字节） | 红线（字节） |
|---|---:|---:|
| minimal | 91,124 | 552,960 |
| hw_factory_intake | 449,268 | 552,960 |
| hw_flash_persist | 98,040 | 552,960 |
| hw_wifi_netmon | 280,120 | 552,960 |
| hw_cloud_chat | 481,648 | 552,960 |

逐镜像单独运行检查器并检查其退出码，随后按地址对拍目标 ELF
`size -A` 输出段；上表是 RF1 WP4a（D12 日额与平台端口）源码的构建结果，本次未烧录；
`hw_cloud_chat` 增加了 KV 持久适配器（`kv.c`、`guard_store.c`）。
该计数不测实际运行中的堆使用、栈峰值或无线/云行为；历史
r15-cloud 的约 403 KB 只列出了两个数据段，不是整个静态 HP SRAM 占用。

## 3. 栈/堆配置实测（r15-cloud 基线；★=真机踩坑定案值）

| 项 | 值 | 依据/教训 |
|---|---|---|
| CONFIG_MAIN_STACK_SIZE | 32768 | TLS 证书解析+RSA 验签深路径（12K 实测崩→16K 仍崩→与下项合因后稳） |
| ★ CONFIG_NET_TCP_WORKQ_STACK_SIZE | 8192 | **默认值在 TLS 记录重组路径溢出=FATAL 2 Stack overflow（HW 栈保护实证，线程 tcp_work）** |
| ★ CONFIG_ESP_HOSTED_MCU_EVENT_TASK_STACK_SIZE | 16384 | 驱动自述"RPC 帧栈上解码须容最大帧"；4096 默认在大流量下溢出（防御性，与 tcp_work 同轮实证链） |
| CONFIG_MBEDTLS_HEAP_SIZE | 65536 | TLS1.3 会话+证书链（32K 时代码路径未达，未单独定界——收窄实验留后续优化批） |
| CONFIG_HEAP_MEM_POOL_SIZE | 131072 | hw_wifi_netmon 实测底（SDIO/nanopb 分配面） |
| CONFIG_NET_RX_STACK_SIZE / sysworkq | 4096 / 4096 | 旧树 E1 实测纪律 |
| CONFIG_ISR_STACK_SIZE | 2048 | 默认 |
| NET_PKT RX/TX、BUF RX/TX、BUF_DATA | 16/16、48/48、512 | F-1 同源（1400 实验后回落——见 §5 教训） |
| MBEDTLS in/out content | 4096/4096 | 证书单帧 ~3.5KB |

## 4. 运行时实测数据点（供 D12 阈值裁决）

| 量 | 实测值 | 条件 |
|---|---|---|
| estop 按下→输出物理坍缩 | 稳态电平定性 <0.1V（万用表，r14）；停止延迟未测量，不能由同步写入路径推导定量上界 | hw_level_probe P3b；EVT_ESTOP 的补发时刻不是物理停止时刻。定量延迟需另行授权并使用合适仪器测量 |
| estop 闩→clear 释放 | 无 clear 命令面（后续批） | — |
| MWDT 硬件复位 | 8.8s（实测）→r02 记录 | hw-gate P2 |
| TLS1.3 完整握手+chat 往返 | 两轮成功（reply=ok usage 24+80/24+84） | r15 真机；延迟未单独计时（留阈值批） |
| WAMR 指令配额 | 默认 1×10^8，并同为实际配额上限（min(请求换算, 确认步数, 1×10^8)）；D12 v1 落地后取代原 E-2 的 2×10^9 clamp | wasm_host.c |
| 唤醒时间 | **无睡眠面**（defer——板无 deep-sleep 实现，测量待产品定义） | — |

## 5. 教训沉淀（配置面"为什么"）

1. **tcp_work 栈**：TLS 记录分片重组路径深于默认假设——HW 栈保护
   （HW_STACK_PROTECTION+STACK_SENTINEL）是快速定案工具（乱跳型
   CPU exception 的第一诊断步）。
2. **esp_hosted RX 栈**：RPC 栈上解码设计（驱动 Kconfig 自述），大流量
   前置放大。
3. **NET_BUF_DATA_SIZE≠越大越好**：1400 实验无收益且偏离 F-1 验证值。
4. **TLS1.3 RSA 证书对端**：必须 offer rsa_pss_rsae（X509_RSASSA_PSS_SUPPORT），
   pkcs1-only 客户端被规范拒（openssl -sigalgs 复现 alert 40）。
5. **PSA 能力发现**：PSA_CRYPTO_ENABLE_ALL 一把梭可跑通；收窄留优化批
   （内存表将随之更新）。
6. **mbedtls 4.x on Zephyr**：符号名与 3.x 习惯不同（无 MBEDTLS_SSL/
   KEY_EXCHANGE_ALL 聚合符号；PROTO_TLS1_3/X509_CRT_PARSE/PEM_PARSE_C
   逐项开）；PSA 依赖链 PSA_CRYPTO→PROVIDER_MBEDTLS→BUILTIN。

## 6. PSRAM 与内存布局（owner 2026-11-24 裁决，尚未落实）

裁决（见规划 10.6）：有 PSRAM 则必须开启；有 PSRAM 时内部 SRAM 只放驱动
固定缓冲区、内核核心、main 线程栈等，其余全部放 PSRAM；内存红线改为三档
（最低、紧凑、宽松），按总内存需求定义。原三档定义不在仓库及其历史中，
三档的口径与数值待所有者定案，现有单一红线在定案前继续生效。

### 6.1 落实布局前必须先核实的技术约束

“SRAM 只留内核、驱动缓冲和 main 栈”不能只按对象的归属来分，下面几类
数据即使属于“其他”也必须或可能必须留在 SRAM：

1. **flash 写擦期间的代码与数据**。XIP 镜像做 flash 写擦时 flash cache 会
   关闭，这段时间要运行的代码（flash 驱动及其调用链）和它碰的数据必须在
   IRAM/SRAM。执行 flash 写入的线程，其栈也在这段时间被使用：除非真板上
   证实 PSRAM cache 不随 flash 操作关闭，这类线程的栈不能放 PSRAM。
2. **中断处理触碰的数据**。中断处理运行在 SRAM 的中断栈上，它访问的对象
   须在 SRAM，或同样满足上一条。
3. **DMA 缓冲**。取决于 DMA 主设备能否访问 PSRAM。SoC 能力表声明 PSRAM
   可被 DMA 访问，但 esp_hosted 的 SDIO 缓冲等须逐项核实。
4. **性能**。热路径数据放进 PSRAM 会变慢，幅度未测。

以上四条都还没有在真板上验证过：PSRAM 现在已经能初始化了（根因与修法见
[lua-feasibility.md](lua-feasibility.md) 的“PSRAM”一节），但这些约束还没有逐条实测
（见
[lua-feasibility.md](lua-feasibility.md)：六种配置均报 PSRAM 未连接，板子
尚未断过电）。

### 6.2 现状：静态 HP SRAM 的构成（按库，输入段求和）

| 镜像 | 输出段合计 | 构成 |
|---|---:|---|
| hw_cloud_chat | 481,648 | 内核库 187,298（内核堆池 131,426；main 栈所在的 `init.c.obj` 38,608；系统工作队列栈 5,118）；网络协议栈 80,194（`net_pkt` 54,432）；mbedtls 堆 65,544；HAL/SoC/驱动 48,154；应用 19,301（通道表 17,952）；esp_hosted 核心 18,372 |
| hw_factory_intake | 449,268 | 应用 291,477（Wasm 宿主 262,161、通道表 17,952、存储 8,372）；HAL/SoC/驱动 41,741；内核 25,382 |
| hw_wifi_netmon | 280,120 | 内核 158,981（内核堆池 131,422）；HAL/SoC/驱动 48,132；网络协议栈 25,704；应用 18,998；esp_hosted 核心 5,372 |

按裁决的原则，这三个镜像里可挪进 PSRAM 的（各类堆、网络缓冲、应用数据、
非 main 线程栈）占了大部分；真正要留下的内核代码、驱动数据、esp_hosted
缓冲和 main 栈合计估计在 160–180 KB 量级。这是估计，未实测。

同一组数字换成按库看（输入段求和，仅供定位）：`hw_cloud_chat` 的 435,891 B
里内核库 187,298 B（其中内核堆池 131,426 B）、网络协议栈 80,194 B、mbedtls
堆 65,544 B、HAL/SoC/驱动 48,154 B、应用 19,301 B、esp_hosted 核心 18,372 B；
`hw_factory_intake` 的 363,291 B 里应用 291,477 B（Wasm 宿主 262,161 B）。

### 6.3 链接层实验（hw_cloud_chat，仅构建参数，未改源码，未运行）

启用 `CONFIG_ESP_SPIRAM=y`，并用上游的 `CONFIG_ESP_SPIRAM_BSS_RELOC_LIBS_AND_OBJS`
把应用、网络、TLS 各库以及 `mempool.c.obj`、`system_work_q.c.obj` 的 `.bss`
指定进 PSRAM：交叉编译退出码 0，`check_board_ram.py` 通过，静态 HP SRAM
从 481,648 降到 **366,592** B，`.dram0.noinit` 仍有 270,948 B，PSRAM 的
`.ext_ram.data` 为 1,173,072 B（含上游预留的 1 MB 共享堆）。

结论：上游现成机制**只能搬 `.bss`**。线程栈、内核堆池、网络缓冲这些大头在
Zephyr 里是 `.noinit` 对象，需要另行处理（链接片段或段属性）；libc 的
`malloc` 也没有被接到 PSRAM 上，要改走 PSRAM 的堆。

### 6.4 落实顺序

1. 真板上 PSRAM 初始化成功（2026-11-24 已完成：根因是板级定义漏了叠封 PSRAM 的
   供电——芯片内部 LDO 的 2 号通道；已按上游做法补进板级定义（设备树 `ldo2@2`
   + `CONFIG_REGULATOR=y`），Zephyr 与 ESP-IDF 6.1 都能正常初始化，
   见 [lua-feasibility.md](lua-feasibility.md)）。
2. 在真板上核实 6.1 的四条约束。
3. 落实布局：`.noinit` 对象与线程栈进 PSRAM、libc `malloc` 改走 PSRAM 堆，
   逐镜像实测 SRAM 与 PSRAM 的占用。
4. 按布局后的实测值定三档红线的口径与数值，改 `tools/check_board_ram.py`。

### 6.5 按总内存的评估（owner 2026-11-24：不限于本芯片）

口径：芯片总内存 M = 内部 SRAM + 已启用的 PSRAM。镜像的总内存需求 D 取实测的
静态占用（本板为静态 HP SRAM 合计），已实测的功能增量直接相加。有 PSRAM 时 D
分摊在两种内存上，线只看总量，SRAM 里放什么由 6.1 的布局原则另行约束；没有
PSRAM 时 M 就是 SRAM。

三档的余量由所有者在 2026-11-24 确认按本表定案。前两档各取一个现有锚点，
第三档是新的：

| 档位 | 条件 | 余量来源 |
|---|---|---|
| 最低 | M ≥ D + 64 KB | D12 已有的运行时堆余量 ≥64 KB |
| 紧凑 | M ≥ D ÷ 0.70 | 原单一红线用的 70% |
| 宽松 | M ≥ 2 × D | 需求不超过总内存的一半（新） |

按本板实测的 D 算出的三条线（字节）：

| 配置 | 需求 D | 最低 | 紧凑 | 宽松 |
|---|---:|---:|---:|---:|
| minimal | 91,124 | 156,660 | 130,178 | 182,248 |
| hw_flash_persist | 98,040 | 163,576 | 140,058 | 196,080 |
| hw_wifi_netmon | 280,120 | 345,656 | 400,172 | 560,240 |
| hw_factory_intake | 449,268 | 514,804 | 641,812 | 898,536 |
| hw_cloud_chat | 481,648 | 547,184 | 688,069 | 963,296 |
| hw_cloud_chat + Lua（嵌套上限 200，配额 128 KB） | 1,005,936 | 1,071,472 | 1,437,052 | 2,011,872 |

Lua 的增量是真机验证过的组合：线程栈 320 KB、解释器堆 192 KB。换成配额 512 KB
（线程栈与解释器堆各 1 MB）的组合时，**单独的 Lua 量测镜像**已在真机上随 PSRAM
实测通过（SRAM 90,524 B + PSRAM 3,146,368 B，见 6.5 末尾）；它与 `hw_cloud_chat`
合在一个镜像里的占用还没有实测，上表最后一行因此仍按 128 KB 配额的组合计。

几种总内存下各配置落在哪一档：

| 总内存 | 五个现有镜像 | hw_cloud_chat + Lua |
|---|---|---|
| 本板 SRAM 768 KB（PSRAM 不可用） | minimal、flash_persist、wifi 为宽松；factory、cloud 为紧凑 | 不够（连最低线也差 300 KB） |
| 仅 SRAM 1 MB | 全部宽松 | 不够（最低线 1.02 MB） |
| 仅 SRAM 2 MB | 全部宽松 | 宽松 |
| 仅 SRAM 4 MB | 全部宽松 | 宽松 |
| 本板 SRAM + PSRAM（约 32.75 MB） | 全部宽松 | 全部宽松（PSRAM 已实测可用） |

由此得到的结论：

- 这块板子不用 PSRAM 也放得下现有五个镜像；需要 PSRAM 的是按上限 200 接入
  Lua 之后的组合。
- 没有 PSRAM、SRAM 不小于 2 MB 的芯片，对已测的全部配置都落在宽松档，不需要
  PSRAM。

这份评估的边界：D 是在本芯片上量的静态占用，移植到别的芯片要重测；运行期的
动态用量没有计入 D，由三档的余量承担。

检查工具已按本标准改写：`tools/check_board_ram.py` 从映射文件读取 PSRAM 区域
（`ext_dram_seg`，没有则为 0），把 SRAM 与 PSRAM 里的输出段都计入 D，逐档
判定；镜像默认须满足紧凑档（`--tier` 可改），否则退出码 1。对上表各配置重新
运行的结果与上表一致：五个产品镜像满足紧凑档，factory 与 cloud 不满足宽松档；
Lua 的 SRAM 量测镜像（638,448 B）不满足紧凑档；Lua 的 PSRAM 镜像（SRAM
90,524 B + PSRAM 3,146,368 B，总内存 34,340,864 B）三档都满足。

板级默认配置加入稳压器驱动之后（§7，2026-11-24），五个产品镜像已用新的板级
定义重新交叉编译（**只编译，未烧录、未运行**），静态 SRAM 合计变为：

| 镜像 | 原 D | 现 D | 增量 | 档位 |
| --- | ---: | ---: | ---: | --- |
| minimal | 91,124 | 91,256 | +132 | 紧凑 |
| hw_flash_persist | 98,040 | 98,172 | +132 | 紧凑 |
| hw_wifi_netmon | 280,120 | 280,196 | +76 | 紧凑 |
| hw_factory_intake | 449,268 | 449,272 | +4 | 紧凑（不满足宽松） |
| hw_cloud_chat | 481,648 | 481,660 | +12 | 紧凑（不满足宽松） |

增量是稳压器驱动的数据，各档归属不变。最新上板镜像（LDO 通道 3/4 显式关闭后）
为 SRAM 90,520 B + PSRAM 3,146,368 B，总内存需求 3,236,888 B。

这五个产品镜像现在都带稳压器驱动（PSRAM 的供电已打开），但**都还没有启用**
`CONFIG_ESP_SPIRAM`：所有者“有 PSRAM 则必须开启”这条裁决目前只在 Lua 量测
镜像上落实，产品镜像的启用与内存布局属于 6.4 的后续工作。

### 6.6 malloc 与动态内存：调研与链接级验证（2026-11-24，未上板）

所有者要求先调研 `malloc` 的解决方案。以下均为读钉定源码、反汇编已构建的镜像、
以及在全新构建目录里**只编译不烧录**得到的结论；运行期行为都还没有在真板上验证。
实验源码与日志在仓库外的 `~/workspace/sandbox/malloc-research/`。

**现状：这块板上有四套互不相通的动态内存。**

| 池 | 接口 | 现在在哪 | 大小 | 谁在用（按最终镜像的反汇编） |
| --- | --- | --- | ---: | --- |
| libc 堆 | `malloc`/`realloc`/`free`/`calloc` | `_end` 到 `_heap_sentry` 的剩余 SRAM | 161,216 B（cloud）到 551,744 B（minimal） | 几乎没人：三个产品镜像里实际使用的只有 nanopb 的 `realloc`/`free`；`hw_wifi_netmon` 连 `malloc` 都没链接进来 |
| 内核堆 | `k_malloc`/`k_calloc` | `.noinit` 段里的静态数组 | 131 KB（cloud、wifi） | HAL 的中断分配、flash 地址映射、定时器；`getaddrinfo`；网络缓冲 |
| 共享多堆窗口 | `shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL)` | PSRAM | 默认 1 MB（`CONFIG_ESP_SPIRAM_HEAP_SIZE`） | 现有产品镜像无人使用 |
| 各模块自带的静态池 | 模块内部 | SRAM | TLS 64 KB、WAMR 256 KB、`net_pkt` 等 | mbedtls、WAMR、网络协议栈 |

产品镜像链接的本工程代码（`framework/`、`platform/`）不调用任何动态分配接口（Lua
量测用的是自建的专用堆，不在产品镜像里）。

两点容易想错的地方：

- **HAL 的 `heap_caps_*` 在 Zephyr 移植里不看 `caps` 参数**（`heap_caps_zephyr.c`），
  要 PSRAM 还是 DMA 内存都一律 `k_malloc`，落在 SRAM 的内核堆。ESP-IDF 那套按能力
  分配的堆在这份移植里并不存在。
- **libc `malloc` 不是 SRAM 紧张的原因**。它现在几乎没人用，搬进 PSRAM 解决的是
  “大块动态内存用不上 PSRAM”，腾不出多少 SRAM；SRAM 里的大头是上表第 2、4 行。

**把 libc 堆放进 PSRAM：两条路，都在链接级验证过。**

1. **静态堆区加链接片段**（实验 E1，成立）。`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE`
   取正数时，堆区是 `lib/libc/common/source/stdlib/malloc.c` 里唯一的 `.noinit` 对象；
   用 `zephyr_linker_sources(RAM_SECTIONS …)` 加一个四行片段把它收进
   `ext_dram_seg` 即可（`RAM_SECTIONS` 片段排在 SRAM 的 `.dram0.noinit` 之前，先匹配
   到）。实测 16 MB 的堆区落在 0x48000000，钉定的 Zephyr 一行未改。库内部对
   `malloc` 的调用不用管，调用的仍是原生实现。代价：堆区大小在链接时定死并计入
   静态需求；没有 SRAM 回退。
2. **两个堆加策略函数**（实验 E2）。在应用里直接定义 `malloc` **行不通**：Zephyr
   第一遍链接（`zephyr_pre0.elf`）把所有 Zephyr 库放进 `--whole-archive`，同名
   定义必然冲突。可行的做法是 `-Wl,--wrap=malloc/free/realloc/aligned_alloc` 加
   `sys_multi_heap`（`CONFIG_MULTI_HEAP`），按策略选 SRAM 池或 PSRAM 池。反汇编
   确认 picolibc 内部（`strdup`、`asprintf`）和 Zephyr 的 `calloc`、`reallocarray`
   都转到了新入口；picolibc 的全部分配入口最终只落到 `malloc`、`free`、`realloc`。
   代价：多约 140 行分配器代码，依赖链接器的 `--wrap`；漏掉某个入口不会报错，而是
   两个分配器并存。

**TLS 堆不需要自己想办法。** 上游已有 `CONFIG_MBEDTLS_HEAP_CUSTOM_SECTION`，配合
`CONFIG_ESP_SPIRAM=y` 即进 PSRAM。在 `hw_cloud_chat` 上的链接结果（只编译）：TLS 堆落在
0x48000000，静态 SRAM 从 481,660 降到 425,612 B，三档全过。

**落实时必须知道的约束。**

- **启动时序**。从镜像的初始化表读出的顺序是：`statics_init`（所有
  `K_HEAP_DEFINE` 的统一初始化，PRE_KERNEL_1 优先级 30）→ `esp32p4_psram_init`
  （优先级 40）→ `malloc_prepare`（POST_KERNEL 35）→ TLS 堆初始化（POST_KERNEL
  40）。所以 libc 堆和 TLS 堆进 PSRAM 是安全的；而任何 `K_HEAP_DEFINE` 的缓冲区
  若放进 PSRAM，会在 PSRAM 映射之前被初始化。Lua 实验没出事是因为它自己在线程里
  显式调了 `k_heap_init`。
- **SDIO 缓冲必须在 SRAM**。Zephyr 的 SDHC 驱动对不在 `0x4ff00000`–`0x4ffc0000`
  的数据缓冲直接返回参数错误（`ESP_ERR_INVALID_ARG`），没有中转；esp_hosted 驱动用的是自己的静态缓冲，
  不受分配器影响。芯片能力表声明 PSRAM 可被 DMA 访问（SPI、GDMA、LCD 等），但
  那是另一条路径。
- **flash 读写用 PSRAM 缓冲不会出错但会变慢**：cache 关闭期间只有 DRAM 可读，
  `esp_flash_read` 经临时 DRAM 缓冲中转，`esp_flash_write` 每次只从栈上搬 32 字节。
- **映射范围**：HAL 在初始化时一次性映射整块可用 PSRAM（代码如此），但真机证据
  只覆盖到前 3 MB 左右，更大范围要用到时应先实测。
- 全新构建目录构建带 nanopb 的镜像时，`.venv/bin` 必须在 PATH 最前（生成步骤需要
  其中的 grpcio-tools），见 `build-support/README.md`。

上游方面：钉定的 Zephyr 里没有“libc 堆进外部内存”的现成选项（读源码确认）；
网络检索到上游有一个相关议题（#120445）和一次讨论（#68568），页面未能打开核对，
合并状态未知。

**跨芯片（所有者追问：改动不能随芯片重来）。** 上面“静态堆区加链接片段”那条路把
ESP32-P4 的链接区域名写死了，换芯片要重写，不能作为长期形态。读钉定源码确认，换芯片
时实际在变的是三件事：

1. 大内存在链接脚本里怎么来。ESP32-P4 是固定常量加一个配置项；ESP32-C5 取自设备树
   节点 `ext_ram`；整棵树里只有少数几家 SoC 的链接脚本会从设备树生成内存区域，
   Espressif 的全都不会。所以凡是在链接时搬静态对象的做法，都要按芯片家族各写一段。
2. 大内存什么时候可用。ESP32 在 SoC 初始化代码里（PRE_KERNEL_1）；ST、NXP 等是
   内存控制器驱动，登记在 POST_KERNEL 的设备优先级（`memc_stm32_ospi_psram.c`、
   `memc_mcux_flexspi.c`）。同一个 `esp_psram.c` 里 P4 走 `SYS_INIT`、C5 由
   `soc.c` 调用，连入口位置都不一样。
3. 哪块内存可以被 DMA 访问、是否经过 cache。

不变的是 Zephyr 的设备树内存区域（`zephyr,memory-region` 加 `zephyr,memory-attr`）：
钉定树里有 18 家厂商的开发板目录在用同一套写法，运行期用 `mem_attr_get_regions()`
读出，与架构无关的“软件属性位”（`ATTR_SW_ALLOC_CACHE`、`ATTR_SW_ALLOC_DMA`）表达
这块内存拿来做什么分配。

据此的做法分两层：

- **通用分配器**（约 240 行，不含任何芯片名、地址或链接符号）。libc 入口经
  `-Wl,--wrap` 转入；池子取自设备树区域表，带 `ATTR_SW_ALLOC_CACHE` 的是大池，带
  `ATTR_SW_ALLOC_DMA` 的是驱动可用池。规则是：声明过的池子只从池子分配，没声明的
  种类用原生分配器（也就是只有一块 RAM 的芯片的情形）。池子在 APPLICATION 级登记
  ——Zephyr 规定这一级在 `main()` 之前、所有设备驱动之后，这是对所有板子都成立的
  就绪条件，不需要知道每颗芯片控制器的初始化优先级。区域如何对应到内存留了一个可
  覆盖的入口（仿真器需要，见下）。
- **每块板只有**：一个设备树节点（地址、大小、属性）和该芯片启用大内存的配置项。

同一份源码的验证结果：

| 目标 | 声明的池子 | 结果 |
| --- | --- | --- |
| 仿真器 | 大池 + 驱动可用池 | 运行：82 项检查全过（分配、回读、`realloc`、释放复用、驱动可用池、库内分配的识别） |
| 仿真器 | 不声明 | 运行：82 项全过，全部回退到原生分配器 |
| ESP32-P4 | 0x48800000，20 MiB | 只编译链接：通过。池子表正确；与链接器放进 PSRAM 的内容不重叠；库内部调用已转入；登记排在初始化表最后 |
| ESP32-C5 | 0x42200000，6 MiB | 同上。源码与前一行完全相同，只有设备树节点不同 |

没验证的：任何芯片上的运行期行为；其它厂商的芯片（钉定的 SDK 里只有 riscv64
工具链，只能读源码）；非 Espressif 芯片上控制器的就绪时刻。

验证过程中暴露并改掉的问题，照实记下：初版在没有声明池子时没有回退到原生分配器
（注释里写了回退，代码里没有，是逐项核对镜像行为时发现的；同时发现只声明大池、没声明
驱动可用池时，驱动可用的分配会直接失败而不是用默认内存，已把规则定明确）；初版把池子登记放在
POST_KERNEL 优先级 36，在控制器由驱动启动的芯片上会过早；仿真器里设备树的地址不是
主机指针，初版一写就段错误，所以加了那个可覆盖的入口；仿真器默认选“只支持小堆”的
实现，720 KB 的池子分到 188 KB 就报满，需要 `CONFIG_SYS_HEAP_AUTO=y`。

这个做法带来的三件事要跟着做：

- `tools/check_board_ram.py` 现在看不到设备树声明的池子（它不在映射文件里），要改为
  从 ELF 的 `mem_attr_region` 表读出并计入需求。本次的核对脚本已经能读出这张表。
- 链接器会把对象放进大内存的芯片家族（Espressif、部分 NXP），板级要有一条链接期
  断言，保证链接器放入的内容不越入池子区；其余芯片没有这个问题。
- `-Wl,--wrap` 是 GNU ld 与 lld 的特性，与芯片无关但与工具链有关。替代做法是给
  Zephyr 的 libc 打一个很小的补丁，仓库已有 6 个钉定补丁的先例。

## 7. 厂商资料与硬件连接核对（2026-11-24）

所有者要求从 Gitee 取该板资料（含原理图）核对硬件连接并完善 Zephyr 配置。
资料（仓库在仓库外，资料本身按其 CC BY 4.0 声明引用）：

| 来源 | 内容 |
| --- | --- |
| `gitee.com/osptek/esp32-p4c5-core-board` | 核心板 88-pin 引脚定义表（`pages/引脚定义.md`，与核心板使用指南第 3.2 节同源）、模组 Altium 符号库 `.SchLib`、封装库 `.PcbLib`、3D 模型 |
| `gitee.com/osptek/devboard-esp32p4c5_module` | **底板原理图** `docs/ESP32-P4C5-Module_Schematic_V1.5.pdf`（Ver 1.5，2026-08-29，图页名“ESP32P4模组基础底板”）、V1.3 旧版、PoE-以太网适配板原理图 V1.2、C5 预编译固件 `ESP32-C5_ESP-Hosted_v2.12.9_merged_0x0.bin` |

**核心板自身的原理图厂商未公开**（核心板使用指南第 5 节写“原理图 暂无”），
所以核心板引脚以官方引脚定义表为准，底板连线以 V1.5 原理图为准。

逐项核对结果（本板 Zephyr 定义 ↔ 原理图/引脚表）：

| 功能 | 厂商资料 | 本板 Zephyr 定义 | 结论 |
| --- | --- | --- | --- |
| PSRAM 与其 PLL 供电 | 内部 LDO 通道 2（1.8 V）；ESP-IDF 默认打开 | 设备树 `ldo2@2` + `CONFIG_REGULATOR=y`（上游做法） | 一致，已真机验证 |
| 闪存供电 | 内部 LDO 通道 1（3.3 V，ROM 上电即开） | `ldo1@1` 3.3 V | 一致 |
| LDO 通道 3/4 | 模组只把 VO4 引到 66 脚，底板未接；VO3 未引出 | 显式 `status = "disabled"`（稳压器驱动会给所有默认 okay 的子节点上电） | 一致 |
| 调试串口 | CH343（U9）：TXD→GPIO38（P4 的 RXD）、收 GPIO37（P4 的 TXD）；BOOT=GPIO35，CHIP_PU 由 CH343 的 DTR/RTS 驱动，GPIO36 10K 上拉 | `uart0_default` TX=GPIO37 / RX=GPIO38 | 一致 |
| TF 卡 | 卡座 TF-123-ARP9H17：DATA0..3=GPIO39..42、CMD=GPIO44、CLK=GPIO43、SWITCH(9)=GPIO45（外部 10K 上拉）；数据/命令线各 10K 上拉 | `sdhc0` clk=43/cmd=44/d0..3=39..42；DET 由应用层读 GPIO45（ACTIVE_HIGH，r14 真机实证：插卡后开关断开、物理高） | 一致 |
| C5 配套 | 厂商 `c5-p4-connection.png`：SDIO D0..3=GPIO14..17、CLK=18、CMD=19；P4 GPIO54→C5 CHIP_PU；P4 GPIO6↔C5 GPIO2 | `sdhc1` clk=18/cmd=19/d0..3=14..17；reset GPIO54（gpio1 22） | 一致 |
| 音频编解码 | ES8311（U11）：MCLK=GPIO13、SCLK=GPIO12、LRCK=GPIO10、DSDIN=GPIO9、ASDOUT=GPIO11、I2C SDA=GPIO7/SCL=GPIO8、CE 接地；功放 NS4150B 的 STD=GPIO53（网络名 PACTRL） | `i2s0_default`、`i2c0_default` 与之一致 | 一致（`&i2s0` 仍关闭，阻塞在 Zephyr 侧 GDMA 缺陷，不是本板配置问题） |
| RTC 备份电 | 84 脚 RTC_VBAT，底板接 +3V3 | 未使用（LP 域另计） | 一致 |

底板上**存在但本板定义未建模**的连接（需要时再开，不属当前产品范围）：

| 连接 | 引脚 | 备注 |
| --- | --- | --- |
| I3C 总线 | SDA=GPIO33、SCL=GPIO32 | 1.8 V 电平转换（TXS0102），供 CSI 摄像头 |
| MIPI CSI | CSI_D0/D1、CSI_CLK，RST=GPIO26 | SC2336 / OV2710 摄像头排线 |
| MIPI DSI | DSI_D0/D1、DSI_CLK | 显示排线；`LCD_BL`=GPIO20、`TP_INT`=GPIO21 |
| 原生 USB | Type-C 经 TS3USB221 切换，USB HOST 口带 H5VND5B 防护 | 模组 48/49 脚标为 USB_D_N/USB_D_P；本板控制台用 CH343，未建模 |
| 以太网 | 无板载 PHY | 只有 PoE-以太网适配板（单独原理图 V1.2） |
| 指示灯 | 板载无 | VLED±/LEDA/LEDK 属于显示排线上的屏 |
| 按键 | 板载无机械按键 | BOOT/ESP_RST 由 CH343 的 DTR/RTS 自动控制 |

两点需要留意：

1. 厂商随底板仓库发布的 C5 预编译固件是**经典 ESP-Hosted v2.12.9**（烧 0x0），
   与本工程自编译的 **esp-hosted-mcu 3.x** 不是同一条线，版本契约也不同；不要用
   厂商二进制覆盖当前可用的 C5 固件。
2. 底板原理图 V1.5 相对本板定义成文的 V1.3 只在标注与版本号上变化，本文所引
   连线在两版中一致（2026-11-24 逐脚核对 V1.5 的 TF_CARD、USB TO UART、Audio
   CODEC、P4/C6 下载切换、MIPI 五个功能块）。

