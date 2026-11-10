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
| LP SRAM | 32 KB | LP 域（未用） |
| PSRAM（psram0，叠封） | 32 MB | **Zephyr 面未启用**（esp32p4 无上游 SPIRAM 堆集成；A-1 论证后不依赖） |
| NOR flash（boya） | 16 MB | 分区表 0x2000 布局（storage 1MB@0x120000） |
| RTC VBAT | — | 88-pin pad 84（严禁悬空，接 3.3V） |

## 2. 固件占用实测（riscv64-zephyr-elf-size，text=flash 代码；total_ram=data+bss）

| 镜像（build dir） | 用途 | text | total_ram | 关键段备注 |
|---|---|---|---|---|
| r14-minimal | 安全常驻监督 | 69 KB | 699 KB | bss 含全量 RAM 镜像数组（未裁 NO_RAM_BACKEND 的历史面） |
| r02-hwgate | WDT 硬件闸 | 72 KB | 697 KB | 同上 |
| r14-lvlprobe | 电平测量探针 | 70 KB | 699 KB | 同上 |
| r14-tf-final | TF 对拍探针 | 77 KB | 134 KB | NO_RAM_BACKEND=1（对照行：裁剪后 -565 KB） |
| r03-hwflash | flash 持久闸 | 83 KB | 150 KB | NO_RAM_BACKEND=1 |
| f1-wifi-gate | WiFi/netmon 闸 | 231 KB | 511 KB | esp_hosted+网络栈 |
| d3-gate-board | 工厂闭环（WAMR+验签） | 173 KB | 543 KB | WAMR RISCV32_ILP32F |
| **r15-cloud** | **MCU 真云（TLS1.3）** | **505 KB** | **884 KB（size 计）** | **实测落位 ≈403KB SRAM**：.dram0.noinit 271KB（esp_hosted SDIO DMA 池/TLS 静态缓冲）+.dram0.bss 133KB；size 的 bss 计数含 noinit，非全部占初始化预算——**跑通实证预算内** |

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
| estop 按下→输出物理坍缩 | 定性 <0.1V（万用表，r14）；定量上界=ISR 同步直写（无队列/调度，代码路径级 µs） | hw_level_probe P3b；时间戳面=补发 EVT_ESTOP 上界（WDT 巡检周期，秒级）——**µs 级定量需示波器（defer）** |
| estop 闩→clear 释放 | 无 clear 命令面（后续批） | — |
| MWDT 硬件复位 | 8.8s（实测）→r02 记录 | hw-gate P2 |
| TLS1.3 完整握手+chat 往返 | 两轮成功（reply=ok usage 24+80/24+84） | r15 真机；延迟未单独计时（留阈值批） |
| WAMR 指令配额 | clamp 2×10^9（E-2 定案） | wasm_host |
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
