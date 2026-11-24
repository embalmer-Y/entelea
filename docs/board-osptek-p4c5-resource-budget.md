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
| PSRAM（psram0，叠封） | 32 MB | **Zephyr 镜像未启用**。所钉定的 Zephyr 4.5 带有 ESP32-P4 的 PSRAM 支持（`CONFIG_ESP_SPIRAM`），此前“无上游集成”的说法不成立。2026-11-24 首次启用尝试（owner 裁决 Lua 的分配改用 PSRAM）：镜像可链接，静态 HP SRAM 从 638,448 B 降到 90,380 B，但真板上 PSRAM 不应答，见 [lua-feasibility.md](lua-feasibility.md) |
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

以上四条都还没有在真板上验证过，因为 PSRAM 尚未初始化成功（见
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

1. 真板上 PSRAM 初始化成功（待所有者给板子断电，或确认硬件与原固件配置）。
2. 在真板上核实 6.1 的四条约束。
3. 落实布局：`.noinit` 对象与线程栈进 PSRAM、libc `malloc` 改走 PSRAM 堆，
   逐镜像实测 SRAM 与 PSRAM 的占用。
4. 按布局后的实测值定三档红线的口径与数值，改 `tools/check_board_ram.py`。

