# 迁移来源与依赖

## 导入的代码

- [Tessera 的固定提交](https://github.com/embalmer-Y/Tessera/tree/64a2cb7b2cb010be1951bf4a25e153f41519efad)：
  固件、板级工具、许可及属性；其 Agent 子树独立导入 Forge。
- Kaleido 提交 `3fbc72266b25e4eee1a0667b7d7af4f9e7157ee9`：提取
  `kaleido_{drive,emotion,ignition}` 及原有黄金测试至 `lib/affect/`。
- Galatea 衍生情感代码保留 MIT 许可及署名；TweetNaCl 保留自身来源声明。
- Galatea 研究基线 `a90c094f23a50452cbb8e8786a026c7ebf8ababb`：
  产品职责参考其 HLD、L00–L13 与 ADR-G-0029/0032/0034/0035；
  不复制研究门链为产品完成证明。原仓库保持不变。

直接迁入的[情感数值参考](../lib/affect/tests/reference/galatea.py)来自该
固定提交的 `galatea/subject/affect/dynamics.py` 中 `lowpass`、
`hysteresis`、`saturate` 及 `valence.py` 中 `ValenceUpdate`、`valence_step`。
保留计算签名与函数体，缩减依赖和说明，增加第一方夹具生成入口；
参考部分遵循 [Galatea MIT 许可](../LICENSES/Galatea-MIT.txt)并保留署名。
生成的 C 向量仅验证共享数值语义，float64 参考与 float32 C 输出允许
`1e-6` 绝对误差，不声称跨精度 bit-exact 或完整情感系统等价。

`import-baseline` 标签记录适配前的导入。源代码中的历史 DEC/Q/HLD/LLD
引用应在上面的 Tessera 提交中查找，不代表当前需求。旧计划、研究和会话
记录没有复制到当前文档；原仓库保持不变。

## 外部依赖

| 依赖 | 固定版本或提交 |
| --- | --- |
| Zephyr v4.5.0 | `41d10b59730406bbab4ca9f1fbbe9e96aafb4966` |
| WAMR 2.4.5 | `25bd7eb63e828e4bd242cc9b38d260b4b31c6605` |
| Espressif HAL，P4 构建，与 Zephyr manifest 一致 | `cc81db00233af6020f2fea9fbb0d0098fb3d0d86` |
| FatFS，P4 SD 诊断可选模块，与 Zephyr manifest 一致 | `4f60a12c58c3de4246857ad87c40239ecd93349b` |
| nanopb，C5 RPC 可选模块，与 Zephyr manifest 一致 | `0f6d088e777a33dd7b54297f137ef10d745c5da8` |
| Python protobuf，`p4-net` 可选开发组 | `7.36.2`（完整解析锁定在 `uv.lock`） |
| Mbed TLS，`p4-tls` 可选源码，与 Zephyr manifest 一致 | `c43f34b93797e81fd0257c30004dcd0ae332ae51` |
| TF-PSA-Crypto，`p4-tls` 可选源码 | `765af96a20a2b408474dcd950372ba9af4d26b62` |
| TF-PSA-Crypto drivers，`p4-tls` 可选源码 | `edb74a92e602cd21a218836aff41a0600229978c` |
| esptool，P4 可选开发依赖组 | `5.3.0`（完整解析锁定在 `uv.lock`） |
| zenoh-pico 1.10.1 | `e1ab223a28aaebb5dec1e70d98eab152332f777a` |
| Unity 2.6.1，主机测试 | `cbcd08fa7de711053a3deec6339ee89cad5d2697` |

P04 硬件核对参考 OSPTEK 公开的[屏幕资料](https://gitee.com/osptek/display-tft-10.1inch-800x1280/tree/ada4a103de51ff2ec383fcc8fe744f73eb4b1d2e)
及[底板资料](https://gitee.com/osptek/esp32-p4c5-module-dev-board/tree/dacb40fa25d6400bfa2524c4e6d4ecbf11b7fea5)。
屏幕根索引列出多个 JD9366 型号而未确定触摸控制器，不能据此认定当前
实物料号、初始化顺序或触摸引脚。

`patches/zephyr-esp32-gdma.patch` 保存原 Zephyr 工作区中未提交的 ESP32 DMA
修改，并非上游代码；仅在对应板级验证时审查和应用，native_sim 不覆盖它。
zenoh TLS 补丁位于 `firmware/module/tessera/ext/zenoh-pico-tls.patch`。

`patches/zephyr-task-wdt-setup.patch` 是针对上述固定 Zephyr 提交的已批准
最小修复：`task_wdt_add()` 原先忽略 `wdt_setup()` 返回值，仍标记后备已
启动并返回通道。补丁传播启动错误、释放失败占用的通道，并保持成功路径。
setup 自动应用到本项目副本，严格核对固定提交及全部批准差异，不修改共享
SDK、HAL 或外部 Zephyr。`core` 的驱动 API 测试经过真实 task watchdog
验证错误传播；这不代表补丁已合入上游或 P4 复位已实测。升级 Zephyr 时须
复核该修复是否已存在，并同步准备入口与回归；不能无条件继续套用旧补丁。

`patches/zephyr-c5-scan.patch` 是同一固定提交的已批准扫描修复：
改用非阻塞 `WifiScanStart`，避免 RPC 等待完整扫描及缺少完成事件；扫描完成
等待经批准由 10 秒调整至有界 20 秒，RPC 上限仍为 5 秒。
实物 C5 v3.0.10 的重复扫描约需 11.6 秒；检查远端启动/完成/读取错误，
记录失败，AP 记录读取失败仍释放 nanopb 分配，不返回成功回调。
ESP-IDF [C5 扫描事件说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/wifi-driver/overview.html)
明确阻塞扫描不产生 `WIFI_EVENT_SCAN_DONE`。升级依赖时须重新评估补丁，
不得继续套用旧超时或由扫描通过推断 TLS、并发及错误恢复已验证。

`patches/zephyr-p4-ram-digest.patch` 是针对同一固定提交批准的 P4 Simple
Boot 兼容补丁：当镜像头声明存在摘要时，loader 在 RAM 校验和后跳过 32
字节，再解析后续 ROM 段；无摘要和其他 SoC 的路径不变。它与第一方镜像
处理共同使用；升级 Zephyr 或 esptool 时须重新核对格式及上游支持。
真机测试中 ROM 对错误摘要给出警告但仍继续启动，不提供故障封闭保证。

`patches/zephyr-cst8xx-report.patch` 是同一固定提交的已批准触摸修复。
主力 P4/CST820 真机在无人触摸时报告手指数和坐标全零，原驱动只检查
事件位，误判为按下，导致诊断无法建立空闲基线。补丁从 `0x02` 一次
读取手指数及 X/Y，零手指始终释放，仍保留原事件位判断和坐标范围。
一次总线读取减少拆开读取 X/Y 的跨报告风险，但不据此宣称历史越界
已经根治。[驱动回归](../firmware/tests/touch_driver/)经 I2C 模拟总线
执行真实驱动，检查事务形状、零报告、陈旧接触、有效事件、越界不裁剪、
总线错误和恢复；它不能替代硬件验证。准备/构建入口均校验此补丁，
依赖准备测试覆盖从原有三项补丁迁移。升级 Zephyr 时须重新评估。

`patches/zephyr-p4-dsi-frame-size.patch` 是同一固定提交的已批准显示修复。
原驱动把缓存对齐后的分配长度用作 DMA 帧长：460×460 RGB565 的真实
423,200 字节被增加到 423,232 字节，DSI 桥却仍按真实尺寸请求像素。
补丁分开真实帧长和分配长度，保留原分配及缓存操作对齐，仅让 DMA
传输真实像素；零长度或不能整除 64 位 DMA 宽度时显式拒绝启动。
参考核对了 ESP-IDF v5.5 的
[DPI 实现](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_lcd/dsi/esp_lcd_panel_dpi.c)
（blob `8372bdd197628f1e34f018ebabe9314e64f718c4`）：
其对齐分配不改变用于 DMA 的逻辑帧长，未复制厂商实现。
准备/构建入口校验此第五项补丁，准备回归覆盖三项、四项及已完整补丁
状态的迁移/重入。板级诊断核对实际 DMA 长度及两种图案的帧完成阈值；
这不替代人工视觉验收。升级 Zephyr 时须复核上游修复和帧长合同。

`patches/zephyr-esp32-gpio-base.patch` 修复同一固定提交的 ESP32 GPIO
高端口访问：`gpio1` 是逻辑端口，P4 DT 的节点基址为 `0x500e0800`，
但 HAL 的整个 `gpio_dev_t` 结构已经包含两个端口，`GPIO_IN1` 在公共
`0x500e0000 + 0x40`，不能再叠加逻辑节点偏移。补丁让两端口的结构
指针均使用既有 `gpio0` 公共基址，保留端口号及 `in`/`in1`、`out`/`out1`
选择；同时修正同一错误机制的读、置位、清零、掩码写和翻转，低端口不变。
准备/构建入口将其作为第六项精确补丁；迁移回归覆盖原五项及重入，
仍拒绝未批准差异。GPIO45 诊断增加 GPIO API 与 HAL 直接输入读取的
一致性检查，真实拔插及其他高端口功能不能仅凭编译或源码推断通过。
升级 Zephyr 时须复核公共寄存器布局与上游修复。

`patches/hal-esp32p4-cpll-config.patch` 是针对固定 HAL 提交、经批准实测
但未解决复位的 P4 Simple Boot 实验候选。真机早期 ROM 标记将额外看门狗复位收窄至
`bootloader_clock_configure()` 内；原路径在等待 CPLL 校准之前，没有
像同版本 Espressif
[参考路径](https://github.com/zephyrproject-rtos/hal_espressif/blob/cc81db00233af6020f2fea9fbb0d0098fb3d0d86/components/esp_hw_support/port/esp32p4/rtc_clk.c)
那样调用 `clk_ll_cpll_set_config()`。
补丁仅补齐该调用，CPLL 目标为既有启动 CPU 频率乘既有分频 4：
当前 v3.x 配置仍为 CPLL 400 MHz、启动 CPU 100 MHz。不改看门狗、
校准等待或电源切换；不是靠降低频率掩盖问题。准备脚本只允许固定 HAL
提交的干净副本，**不再自动应用或要求该候选**：带补丁真机仍发生
异常及看门狗复位，未达到修复判据，本项目 HAL 已仅撤销这一行。
补丁文件只保留实验可复现性，不属于已验收修复；不能仅凭参考路径
差异断言根因。

仅诊断构建使用的
[细检查点补丁](../firmware/tests/board/p4touchid/clock_trace.patch)
针对同一固定 HAL 的 `zephyr/esp32p4/src/soc_init.c`，由
[构建隔离逻辑](../firmware/tests/board/p4touchid/clock_trace.cmake)
校验完整输入摘要后应用于构建目录副本，保留原版权声明。
它只插入第一方 RTC 检查点及校准循环计数，不变更原延时、寄存器
配置、频率或看门狗控制；普通固件不使用该副本，不新增默认依赖补丁。
主机回归覆盖输入变更/缺失/重复、补丁失败、重复配置、逆向恢复原
源码和真实包装/有界软件复位控制流，不模拟 P4 时钟或模拟电源域。

进一步对照同一固定 HAL 的
[rtc_clk_init 参考](https://github.com/zephyrproject-rtos/hal_espressif/blob/cc81db00233af6020f2fea9fbb0d0098fb3d0d86/components/esp_hw_support/port/esp32p4/rtc_clk_init.c)，
可见模拟稳压器控制及供电/时钟交接顺序的差异，但未直接照搬供电写入或
修改校准电压。[官方 v3.2 兼容性公告](https://documentation.espressif.com/AR2026-004_Compatibility_Advisory_for_ESP32-P4_Chip_Revision_v3.2_EN.html)
描述 esptool 的 flash 上电适配及 RTC 看门狗 ROM 补偿变化，不能据此
认定本次先异常、后 HP 系统看门狗复位的根因，也不能用关闭看门狗代替修复。

## 有意排除与适配

[P4 镜像检查器](../scripts/prepare_p4_image.py)是第一方构建适配；针对固定
P4 HAL 地址范围、16 段 loader 上界和 esptool RAM-only 格式，利用首个零
填充段生成 ROM 可见 RAM 镜像的摘要，并追加 8 字节无效段结束标记；
它与上述 loader 补丁配套，不修改 ROM 段位置或支持签名镜像。升级 HAL、
Zephyr 或 esptool 时须重新核对格式、摘要及上游终止处理。

[受控 DNS/TLS 诊断](../firmware/tests/board/p4wifibench/src/tls.c)和
[主机探针](../tools/board/p4_net_probe.py)是第一方测试代码，使用固定 Zephyr
原生 sockets TLS 及临时测试证书，不新增 loader、SDHC 或密码库补丁。
负向 DNS 判据依赖当前 Zephyr 的零 answer 到 `DNS_EAI_FAIL` 映射，升级
时须重新核对并保留实际 NXDOMAIN 证据。此诊断不代表生产时钟、信任配置
或迁入的 Zenoh TLS 适配已经验收。

没有迁入本地凭据、配置、工作区、构建缓存、会话日志及 ESP-IDF 对话/UI
外壳。公开测试签名材料仅作夹具，不得配置到生产设备。

旧环境安装器由隔离源码准备脚本替换，其测试已针对新脚本改写并保留 TLS
补丁完整性检查。Forge 的 gateway 子进程测试使用临时工作区。情感 ignition
空参数测试改为初始化结果并逐项检查不变性；内核行为未改。
