# 开发与贡献

本文面向 Entelea 贡献者。当前验证的主机为 Linux x86-64，使用 Python
3.12、`uv`、GCC、CMake、Ninja 和支持 wasm32 的 clang。两个仓库各自维护
锁文件；不使用其他项目的虚拟环境或修改共享 SDK。

## 主机环境

在已递归初始化子模块的主仓库中：

```sh
uv sync --locked
uv sync --project agent --locked --extra dev
make check
make agent-test
```

预期退出码为 0。情感测试首次可能联网下载固定版本的 Unity；离线时可先通过
`cmake -S lib/affect -B build/affect -G Ninja -DCMAKE_BUILD_TYPE=Release
-DFETCHCONTENT_SOURCE_DIR_UNITY=/absolute/path/to/Unity` 配置已有源码。

## native_sim 固件

以下准备步骤需要网络并将固定版本源码放在本仓库忽略的 `.deps/` 中：

```sh
bash scripts/setup-firmware.sh
make firmware-build
make firmware-test
```

预期构建和测试退出码均为 0。已有源码可以只读使用：

```sh
export ZEPHYR_BASE=/absolute/path/to/zephyr
export TS_WAMR_DIR=/absolute/path/to/wamr
make firmware-test
```

外部源码有修改或版本不一致时，不等同于固定基线。板级交叉构建还需要 Zephyr
SDK、对应 west 模块和可能的厂商 blob；主机验证不能替代板级验证。参见
[测试指南](testing.md)与[板级工具说明](../tools/board/README.md)。

setup 会在本项目的固定 Zephyr 副本应用经过批准的
[看门狗启动补丁](../patches/zephyr-task-wdt-setup.patch)、
[C5 扫描补丁](../patches/zephyr-c5-scan.patch)、
[P4 RAM 摘要补丁](../patches/zephyr-p4-ram-digest.patch)、
[CST8xx 报告补丁](../patches/zephyr-cst8xx-report.patch)及
[P4 DSI 帧长补丁](../patches/zephyr-p4-dsi-frame-size.patch)、
[ESP32 GPIO 寄存器基址补丁](../patches/zephyr-esp32-gpio-base.patch)，重复执行可复用，
也支持从原先部分补丁的状态迁移。它仅接受固定提交加批准补丁的精确差异；
其他修改、暂存内容或未跟踪文件会明确失败，不覆盖已有修改。
P4 HAL、WAMR 和 nanopb 必须是固定的干净源码。
[CPLL 配置候选](../patches/hal-esp32p4-cpll-config.patch)未解决真机启动
复位，仅保留实验出处；准备和构建入口均不应用或要求它。已手工应用过
该候选的副本须先核对并仅撤销该精确改动；准备脚本不会覆盖脏树。
只读复用的外部 Zephyr 也必须已经具备这六个补丁；构建入口在调用工具前检查，
不会自动修改外部源码。补丁背景见[依赖出处](provenance.md#外部依赖)。

## ESP32-P4/C5 交叉构建

已有 Zephyr SDK 可只读复用，不修改共享安装。以下入口仅生成 P4 框架镜像，
不访问串口、不烧录、不配置 C5，也不调用模型：

```sh
uv sync --locked --group p4
bash scripts/setup-firmware.sh p4
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-build
```

P4 可选依赖组锁定 esptool 5.3.0；固定 HAL 的要求即为 `esptool>=5.3.0`，
镜像生成也需要该工具。setup 在忽略的 `.deps/` 内准备与固定 Zephyr
manifest 一致的 `hal_espressif`；不安装 SDK 或厂商 blobs，不触碰历史项目。
SDK 必须包含 RISC-V 工具链；当前构建使用 Zephyr SDK 1.0.1 / GCC 14.3.0。

入口固定目标 `esp32p4c5_core_board/esp32p4/hpcore`，使用
`ZEPHYR_TOOLCHAIN_VARIANT=zephyr` 和独立 `build/firmware-p4/`，显式接入
框架与 HAL。`make firmware-*` 的原有 native_sim 行为不变。如只读复用
其他源码，可设置 `TS_HAL_ESPRESSIF_DIR`，但非固定版本或脏树不等于固定
构建基线。新增网络/多媒体配置所需的模块或 blobs 另按对应 manifest 准备，
不能将本入口当作完整产品配置或 C5 联网验证。

构建后检查 `build/firmware-p4/CMakeCache.txt` 的编译器路径、
`build/firmware-p4/zephyr/.config` 的 `CONFIG_SOC_ESP32P4=y` 以及
`build/firmware-p4/zephyr/zephyr.elf` 的 RISC-V ELF 头。框架默认配置使用
RAM 存储后端；P4 bring-up 片段明确采用两个 32 KiB RAM 槽位和 16 KiB App
线程栈，保留 WAMR 与安全层，当前仍为默认 16 KiB 装载上限的小 App 档。
默认两个 256 KiB 槽位加 256 KiB WAMR 池无法放入内部 SRAM，不能把主机
预算直接用于 P4。该配置不宣称持久化、大 App 或 PSRAM 已验证；目标板
flash 分区、App 持久化、PSRAM 和媒体接线属于后续模块，不能由镜像生成
成功推断已经接通。

该板描述迁自历史核心板适配。用户已进一步确认 v3.2 是芯片硅版本，而非
底板 PCB 修订；底板按厂商最新公开资料核对。构建不证明屏幕、触摸、
摄像头或其他接线已经验收，不得将本入口直接当作烧录授权。

### Simple Boot 镜像摘要与结束保护

本板的 CMake [镜像钩子](../firmware/boards/osptek/esp32p4c5_core_board/image.cmake)
由框架模块在 SoC 注册 esptool 转换之后接入，在 `zephyr.bin` 生成后自动运行
[P4 镜像检查器](../scripts/prepare_p4_image.py)，所有上述 P4 入口及直接
CMake 构建均覆盖；修改检查器也会触发最终镜像重新生成。
它针对 P4 Simple Boot RAM-only 格式验证段边界、RAM 校验和、地址范围和
段数。对无摘要输入，在首个零填充段内放入 ROM 可见 RAM 镜像的 SHA-256，
相应缩短填充并设置头部标志，不移动 ROM 段或改变 ELF；填充不足或非零时
明确失败。已有摘要必须验证正确，再追加 8 字节 `0xff` 无效段结束标记；
重复执行不增长镜像。项目内 Zephyr 补丁使 P4 loader 在摘要标志为 1 时
跳过这 32 字节。其他启动格式保持原有构建行为。

Simple Boot 从 `boot_partition` 起始地址烧录，本板当前为 `0x2000`；
包含标记的镜像必须止于 `zephyr,code-partition` 的末尾之前或恰到末尾，
当前上界为 `0x7e0000`，不能侵入备用槽或存储。范围由当前 devicetree
计算，不硬编码整个 Flash 容量。仍必须核对具体设备身份、备份、实际擦除
范围并取得烧录授权。

没有标记时，镜像恰在擦除扇区边界结束会让 loader 继续读取上一版较大镜像
的零填充残留，最终超过段数上界而无法启动。此修复明确终止镜像，不依赖
额外全片擦除或提高段数。摘要兼容补丁仅应用于项目固定 Zephyr 副本，
不修改共享 SDK 或 esptool。新板上的正反验证见[测试指南](testing.md#c5-扫描与网络验收)：
摘要只覆盖 ROM 可见的 RAM 段；ROM 在摘要错误时仍继续启动，它不是全镜像
完整性校验、签名或 Secure Boot，不能用于断言损坏时故障封闭。

### 无凭据 C5 扫描配置

```sh
uv sync --locked --group p4-net
bash scripts/setup-firmware.sh p4-net
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-net-build
```

需要主机已有 `protoc`；该可选组通过锁文件固定 Python protobuf，
setup 准备与固定 Zephyr manifest 一致的 nanopb。普通 native/P4 入口不
接入 nanopb，也不修改共享 SDK；外部 nanopb 可通过 `TS_NANOPB_DIR` 只读复用。

入口固定实际 P4/C5 板，输出独立的 `build/firmware-p4-net/`。
[扫描配置](../firmware/tests/board/p4wifibench/boards/esp32p4c5_core_board_esp32p4_hpcore.conf)
使用直接 UART 日志保留 C5 启动版本，不含网络凭据，不关联 AP、不写入
存储、不调用模型；保留硬件看门狗，但不是完整框架启动配置。
扫描补丁改用非阻塞启动及有界 20 秒完成等待，RPC 保持 5 秒；
失败或空结果立即终止本次诊断，避免超时后重试误收迟到事件。
判据和未覆盖范围见[测试指南](testing.md#c5-扫描与网络验收)。

### P4 触摸芯片识别诊断

```sh
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-touch-id-build
```

使用已准备的 P4 依赖，独立输出到 `build/firmware-p4-touch-id/`，构建不
操作设备。诊断只在板级 I2C0 上读取 `0x15` 设备的 `0xa7` 芯片标识寄存器；
不会配置触摸复位脚、启用显示或连接 C5 网络。读取过程包含向 I2C 设备
发送寄存器地址，不等同于完全被动监听。`0xb7` 才报告 CST820 标识通过，
总线错误和其他标识均显式失败；未响应不能单独证明屏幕无触摸。
此诊断不启动框架或其必需的硬件看门狗，不是产品固件。
实际部署须确认目标、备份、擦写范围、供电和排线安全，结果与限制见
[媒体与 SD 验证](testing.md#媒体与-sd-验证)。

### P4 触摸事件诊断

```sh
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-touch-build
```

独立输出到 `build/firmware-p4-touch/`。先读芯片 ID，再手动初始化
Zephyr CST8xx 轮询驱动；驱动会修改触摸芯片运行时控制寄存器，不写固件。
不配置中断或复位引脚、不启用屏幕和网络。记录原始坐标、按下与释放，
检查坐标在 `0..459`，等待五个按下/释放对，最长 300 秒。
初始化前及等待空闲失败时，只读快照 `0x00..0x06` 寄存器，辅助区分
芯片报告和事件转换问题。驱动补丁一次读取手指数及 X/Y，零手指必须报告
释放，不能将上电全零报告误判为按下；它不裁剪或隐藏越界坐标。
数到五对只说明接收了符合范围的事件；需与人工按压位置对应后判断定位，
不表示 C5 共存或 App 接入已验收。超时或错误会显式报告；事件报告停止后
驱动轮询仍会运行，需刷回经验证的非轮询诊断或其他已批准固件。
此配置不启动框架或其必需的硬件看门狗，不是产品固件。烧录与实际触摸
操作须先确认用户授权和硬件安全。

### P4 低亮度显示诊断

```sh
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-display-build
```

独立输出到 `build/firmware-p4-display/`；使用固定 Zephyr 已有的通用
DSI 主机和扫描控制器，不启用 NXP 耦合的 CO5300 驱动。
仅适用于已核对的 AM200MT460460FLS2：460×460 RGB565、单 lane
320 Mbps、DPI 8 MHz，窗口 X 偏移 10。LDO3 配置为 2.5 V；
启用稳压器驱动时，还须保留 PSRAM/MPLL 的 LDO2 1.8 V 常供，
不能只配置 DPHY 电源。诊断开启外部 RAM 内存自检。

不驱动屏幕复位脚、不启用触摸、音频、相机或网络。首次真机图案通过后，
用户因过暗而批准普通亮度由 8 提高至 32/255；HBM 仍为 8/255，
不启用高亮模式。RGB 竖条、红绿蓝白四分区各显示 15 秒，
检查各阶段至少 150 次帧完成回调，并从实际 DMA 寄存器核对每帧
52,900 个 64 位传输单元（423,200 字节），不得传输缓存填充；
最后尝试清亮度、关屏、睡眠、
停止扫描并关闭 LDO3，任何失败均报告。LDO2 保持供电供 RAM 使用。
`IO_OK` 只说明有界的命令/扫描检查，仍须人工核对颜色、方向、偏移、
闪烁和熄屏，不能当作视觉验收、零 DMA 错误或长期稳定性证明。
帧完成不足时，先保留失败和只读 DMA/DSI 寄存器快照，再单独运行
10 秒主机内部彩条，以区分面板链路和 RAM/DMA 路径；此隔离步骤始终
标记 `acceptance=0`，不能将原失败转为通过。
构建不烧录；部署及观察须先确认目标、授权、全 Flash 备份与恢复方案。

### P4 早期启动诊断

```sh
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-boot-trace-build
```

此入口复用触摸芯片识别工程，独立输出到 `build/firmware-p4-boot-trace/`，
仅该配置启用 `CONFIG_TS_P4_BOOT_TRACE`。普通识别镜像不包含诊断包装。
ROM 控制台标记 `@H+/-` 为硬件初始化入口/返回；其中 `S` 为 SoC 初始
配置、`A` 为模拟复位配置、`W` 为 super-WDT 配置、`C` 为启动时钟配置。
代码和标记字符串驻 RAM，不依赖尚未映射的 flash rodata 或 Zephyr console。
时钟内 `R`、`D`、`P`、`L`、`E` 分别包装 RC_FAST 开启、ROM 延时、
HP/LP 校准值读取及模拟 I2C 临界区退出。默认只把最后阶段和延时序号/
参数写入 RTC 保留 RAM，不在时钟内部输出串口；看门狗复位后用 `@LAST`
打印最近记录，无有效标记时明确输出 `unavailable`。
细分构建另需 `patch`：仅对构建目录内的 HAL 源码副本应用
[检查点补丁](../firmware/tests/board/p4touchid/clock_trace.patch)，不修改
固定依赖。输入 SHA-256、唯一目标源或补丁应用不满足合同即中止构建；
重复配置重新复制原输入，不叠加补丁。普通识别构建不走此路径。
`@FINE` 记录 `B` CPLL 开启/校准整体、`Q` 校准轮询及其循环次数、
`V` DCDC 交接、`O` LDO 关闭、`a/s/m/c` APB/SYS/MEM/CPU 分频及
各自总线更新、`F` CPU 源/ROM ticks 切换、`M` MSPI 时钟设置。
`+/-` 分别为进入/返回；记录说明最后到达的位置，不证明该阶段是根因。
细检查点只写 RTC；轮询计数也会扰动时序，不是零开销观测。
`@WDT0 at=entry` 是对应启动进入硬件初始化时只读取得的 TG0 WDT
CONFIG0/CONFIG1/stage0 hold；在正常返回、异常或后续 WDT 启动时输出。
`at=after` 在硬件初始化返回后再次读取，不能把它误作复位前现场。
检查真实 P4 的五种看门狗复位原因，不能将 `CORE_MWDT=0x07` 与
`SYS_RWDT=0x10` 混淆。记录是最后到达阶段，不是完整回溯，也不保证
跨掉电、其他镜像或 RTC 域复位保留。
异常时 `@FAULT` 记录 cause/PC/mtval，`@CONTEXT` 记录异常帧的 a0/ra/
mstatus，`@CLOCK` 记录阶段与延时信息，然后仍调用原故障处理；不尝试
返回损坏现场或吞掉异常。`CONFIG_TS_P4_BOOT_TRACE_DETAILS` 默认关闭，
显式打开才增加时钟内部 UART 标记；细输出曾掩盖额外复位，不计稳定性通过。
诊断只增加包装和检查点，不关闭、延长或额外喂看门狗；串口输出会改变启动时序，
因此带标记运行不能替代无标记固件的启动复测，更不等于冷上电或整机 H0。
构建不操作设备；真机部署仍须核对目标、备份、边界及恢复方案。

有界软件复位对照必须使用独立构建目录，默认不启用：

```sh
export ZEPHYR_BASE="$PWD/.deps/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export PATH="$PWD/.venv/bin:$PATH"
cmake -S firmware/tests/board/p4touchid -B build/firmware-p4-soft-reset -G Ninja \
  -DBOARD=esp32p4c5_core_board/esp32p4/hpcore -DBOARD_ROOT="$PWD/firmware" \
  '-DEXTRA_CONF_FILE=prj.conf;boottrace.conf;softresets.conf' \
  -DKCONFIG_WARNING_AS_ERROR=ON \
  "-DZEPHYR_MODULES=$PWD/firmware/module/tessera;$PWD/.deps/hal_espressif"
cmake --build build/firmware-p4-soft-reset --parallel 2
```

`CONFIG_TS_P4_BOOT_SOFT_RESETS` 仅依赖追踪配置；每次识别成功后调用 ROM
CPU0 软件复位，RTC 状态最多允许五次，完成输出 `P4SOFT DONE count=5`
后退出主线程。仅 POWER_ON 原因初始化一轮；CPU_SW 必须有有效计数。
遇 WDT、其他原因、损坏状态或复位函数意外返回均明确 ABORT/FAIL，
不自动重试。此对照不是整芯片软件复位，更不是物理断电测试。

### P4 无外设 hello 对照

直接构建固定 Zephyr 的未修改 `hello_world` 示例，仅加载 Espressif HAL，
不加载 Tessera 模块。使用[无外设配置](../firmware/tests/board/p4hello.conf)
关闭 I2C/I2S/SPI/SDHC、网络/蓝牙、DMA、显示、输入、视频和传感器驱动。
GPIO 支持保留为 ESP32 UART 控制台的必要依赖；应用不操作 GPIO。
不改变既有启动时钟、PVT 或看门狗设置，不包含启动追踪/自动软件复位。

```sh
export PATH="$PWD/.venv/bin:$PATH"
export ZEPHYR_BASE="$PWD/.deps/zephyr"
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
cmake -S .deps/zephyr/samples/hello_world -B build/firmware-p4-hello-bare -G Ninja \
  -DBOARD=esp32p4c5_core_board/esp32p4/hpcore -DBOARD_ROOT="$PWD/firmware" \
  -DEXTRA_CONF_FILE="$PWD/firmware/tests/board/p4hello.conf" \
  -DKCONFIG_WARNING_AS_ERROR=ON \
  -DZEPHYR_MODULES="$PWD/.deps/hal_espressif"
cmake --build build/firmware-p4-hello-bare --parallel 2
python scripts/prepare_p4_image.py build/firmware-p4-hello-bare/zephyr/zephyr.bin \
  --max-size 0x36000
```

没有加载 Tessera 模块时，不会自动引入板级镜像后处理；最后一步仍须
生成固定 Simple Boot loader 要求的 RAM 摘要及结束标记。
此处 `0x36000` 限制仅用于 `[0x2000,0x38000)` 有界诊断，不是通用刷写
许可或生产分区大小。镜像仍使用项目固定 Zephyr/HAL 与已批准 loader
补丁，不应称为完全未打补丁的上游固件。烧录前另行核对新板串口身份、
安全状态及完整备份；构建本身不操作硬件。EN 重复复位不等于冷断电。

### 摄像头 HAL 生命周期

`ts_av_config_bind` 仍为单个 AV 绑定，不是多 App 摄像头仲裁。
它与采集/放音串行执行；主体、格式或尺寸改变时，先停止并回收旧视频流，
成功后才提交新配置。清理失败返回 `TS_E_IO` 并保留旧配置和未回收资源；
后续采集可重试清理，不能继续分配而耗尽缓冲池。相同绑定不无故重启流，
非法参数不改变现有绑定。App 启动装载先检查 AV 绑定，失败不启动该 App。

摄像头分支的[软件回归](../firmware/tests/camera/)按两帧缓冲的所有权测试
初始化失败、停流/刷新/回收失败、配置切换及并发。停止成功并不保证固定
Zephyr 内部刷新成功，HAL 另行检查；部分启动失败也必须重新停流。
所有权尚未归还或无法确认停止时保留内存并报错，不释放潜在 DMA 缓冲。
这不证明真实驱动停流或物理时延，适用范围见[测试指南](testing.md#媒体与-sd-验证)。

### P4 SD 诊断配置

```sh
uv sync --locked --group p4
bash scripts/setup-firmware.sh p4-sd
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
make firmware-p4-sd-build
```

此可选入口在 `.deps/` 准备与固定 Zephyr manifest 一致的 FatFS，输出到
`build/firmware-p4-sd/`；普通 native、P4 和网络入口不加载它。
构建不操作设备。实际诊断会在已授权测试卡上独占创建、写入并删除新文件，
禁用格式化；应先核对设备、卡和恢复方案。测试判据及限制见
[媒体与 SD 验证](testing.md#媒体与-sd-验证)。

### P4 I2S 数据回环诊断

使用已准备的 P4 依赖和 Zephyr SDK：

```sh
make firmware-p4-i2s-build
make firmware-p4-i2s-candidate-build
make firmware-p4-i2s-candidate32-build
make firmware-p4-i2s-stop-build
make firmware-p4-i2s-stop-candidate-build
```

前三个入口分别构建 16 位槽未修改 GDMA 对照、16 位槽候选及 32 位槽
候选；后两个入口构建 32 位槽的普通 `STOP`/重启诊断，分别使用未修改
GDMA 和隔离候选。各自输出到 `build/firmware-p4-i2s*/`，构建本身不操作设备。
[独立诊断](../firmware/tests/board/p4i2sbench/)在运行时以 GPIO9 的
I2S 输出经芯片内部 GPIO 矩阵接回输入，传输并校验双声道递增数据，
数据诊断执行两轮 `DROP` 和重启；停止诊断检查 `STOP` 后 WS 不再翻转、
未发送队列保留、无需重新填充即可连续重启，最后检查 `DROP` 后缓冲
归还。它们不是 ES8311 声学测试或产品音频后端。

候选构建需要 `patch` 工具，只在对应构建目录复制并应用
[归档补丁](../patches/zephyr-esp32-gdma.patch)；配置时校验固定原始
GDMA 源和补丁摘要，任何不匹配均报错。它不更改共享 Zephyr 源，也不
为普通 P4 或 native 构建启用候选；该候选尚未批准用于生产，且未解决
M2M RX0 超时。实测前须核对被授权目标、镜像写入范围、校验和恢复方案；
现有真机结果及未覆盖范围见[测试记录](testing.md#68007-i2s-与音频信号对照2026-11-05独立诊断)。

### P4 音频外部刺激与听音诊断

[p4acoustic](../firmware/tests/board/p4acoustic/)用于板载播放、麦克风
统计和人工听音/遮挡；[p4audiopc](../firmware/tests/board/p4audiopc/)
用于功放关闭、DAC 静音时的电脑双频刺激。两者都只用于隔离试验，
各自在构建目录使用并校验归档 GDMA 候选，不改变普通构建或产品依赖。
`p4acoustic` 固件里的 `PASS acoustic=1` 仅代表其信号阈值满足，不能
代替听音和物理对照；遮挡时阈值不满足也须保留原始结果。

使用已准备的固定 P4 依赖、Zephyr SDK、项目 Python 和 Ninja，在仓库根
目录分别构建：

```sh
export ZEPHYR_BASE="$PWD/.deps/zephyr"
export ZEPHYR_SDK_INSTALL_DIR=/absolute/path/to/zephyr-sdk
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export PATH="$PWD/.venv/bin:$PATH"
for diagnostic in p4acoustic p4audiopc; do
  cmake -G Ninja -S "firmware/tests/board/$diagnostic" -B "build/$diagnostic" \
    -DPython3_EXECUTABLE="$PWD/.venv/bin/python" \
    -DBOARD=esp32p4c5_core_board/esp32p4/hpcore \
    -DBOARD_ROOT="$PWD/firmware" \
    "-DZEPHYR_MODULES=$PWD/firmware/module/tessera;$PWD/.deps/hal_espressif"
  cmake --build "build/$diagnostic"
done
```

构建不操作设备。实测前须核对授权设备、镜像写入范围、校验和恢复方案。
`p4audiopc/experiment.py` 固定使用板卡 `68007` 和电脑的 PCH 模拟
输出设备：须先确认这两个设备均空闲且电脑音频播放获授权，脚本会复位板卡
并播放低幅度合成音，不更改混音设置、不保存原始音频。现有结果和边界见
[音频测试记录](testing.md#68007-音频外部刺激与人工遮挡2026-11-05)。

### P4 SC2336 隔离采帧诊断

[独立 ESP-IDF 诊断](../firmware/tests/board/p4cameraidf/)不是 Zephyr 产品
固件；使用 ESP-IDF 6.1 的 `c55d13c6fbcbf2fc49c0639944134fee96961fbf`
及工程中的组件锁文件。准备对应 IDF 环境后，在诊断目录执行 `idf.py build`。
构建不会操作设备；项目组件由 Espressif 组件管理器按锁文件解析。

烧录前必须核对设备、完整实际写入范围、目标分区和恢复方案；`build/flash_args`
列出本次构建的偏移。当前接收工具仅针对已确认串口尾号 `68007` 且解析
为 `/dev/ttyACM0` 的板，使用系统 Python 的 `pyserial` 和 `Pillow` 及
`fuser`。诊断镜像烧录后，以串口空闲状态运行：

```sh
/usr/bin/python3 capture.py --output /private/existing-directory/new-preview.png
```

工具会独立 EN 复位并采集图片，只在校验字节数、Adler-32、终止标记和
设备清理成功后创建 PNG；拒绝覆盖已有文件及向仓库内写图。请仅在
获授权的场景中采集并私下处理图片。此诊断不验证 Zephyr 相机驱动、产品
HAL 或 App 集成，真实结果和局限见[相机测试记录](testing.md#68007-sc2336-隔离采帧2026-11-05)。

### TLS 能力与运行时联网诊断

```sh
uv sync --locked --group p4-tls
bash scripts/setup-firmware.sh p4-tls
make firmware-p4-tls-build
```

沿用上述 SDK 环境。此可选入口额外准备 manifest 固定的 Mbed TLS、
TF-PSA-Crypto 及驱动源码，输出独立的 `build/firmware-p4-tls/`。
原 native、P4、扫描入口不加载这些密码库。配置显式启用 TCP、TLS 1.2、
ECDHE-ECDSA/AES-128-GCM/SHA-256 和证书有效期支持；TLS 诊断依赖 TCP，
该构建入口将 Kconfig 警告视为错误，防止依赖缺失时静默裁掉验收代码。
**编译成功不等于 TLS 握手、证书拒绝或真实模型验证通过**。

当前[联网诊断](../firmware/tests/board/p4wifibench/src/network.c)先验证 WPA2
关联、真实 DHCP 绑定及显式断开/重新连接，并调用
[DNS/TLS 诊断](../firmware/tests/board/p4wifibench/src/tls.c)。Wi-Fi 与 DHCP
回调按各自事件层分开注册，不混合事件层的掩码；各阶段有界等待，
失败也尝试有界断开并保留原始错误，立即停止后续轮次。总成功标记必须
与各阶段观测及受控主机证据同时成立，不代表完整 P03 或完整框架验收。

运行前必须明确指定并授权目标串口。诊断输出 `P4L CONFIG_WAIT` 后接收
二进制帧：`P4C1`、SSID/PSK 各一个长度字节、对应原始字节；长度分别为
1–32、8–63，输入整体上限 30 秒，固定 WPA2-PSK。不启用串口回显，
不把凭据放进源码、Kconfig、构建参数或缓存，结束时明确清除 P4 凭据缓冲。
配置文件留在仓库外且权限为 0600；采集日志仍须脱敏。既有 C5
`WifiSetConfig` 可能保存配置到其 NVS，不能宣称 C5 凭据仅驻 RAM 或配置
未变化；联网测试应使用获准的专用网络。此入口不升级 C5 固件、不调用模型。

获准部署该诊断镜像后，使用[受控网络探针](../tools/board/p4_net_probe.py)：

```sh
.venv/bin/python tools/board/p4_net_probe.py \
  --run --device "$AUTHORIZED_DEVICE" \
  --wifi-config "$PRIVATE_WIFI_CONFIG" --security wpa2-psk \
  --evidence-dir "$NEW_PRIVATE_EVIDENCE_DIR"
```

三个路径必须预先明确：存在且未占用的稳定 `/dev/serial/by-id/` 目标、
仓库外当前用户所有的 0600 Wi-Fi JSON，以及仓库外尚不存在的证据目录。
JSON 只需 `ssid`、`psk` 字符串，安全类型由命令明确指定。探针不会烧录，
但 `--run` 授权复位和运行时配置；还须事先授权本机受控 DNS/TLS 服务。
需已有 `ip`、`fuser`、`openssl` 及可选组中的串口/esptool 依赖。

主机根据板卡真实 DHCP 地址选择自身路由源地址，仅绑定该地址的临时端口，
只接受本机和目标板地址。生成临时 P-256 测试 CA/服务器证书，先通过
主机正向回显与三种证书拒绝预检，再发送测试端点。每轮 `TLS_CONFIG_WAIT`
接收 `P4T1` 帧：27 字节头包含 IPv4、四个 BE 16 位 DNS/有效/不可信/过期
端口、BE 64 位可信测试 UTC 秒、BE 16 位 DER CA 长度及一字节 hostname
长度，后接 ASCII hostname 和 DER CA。长度分别为 1–63 和 1–1536，
输入整体上限 30 秒；UTC 要通过 2020–2100 及 `time_t` 范围检查。
这是测试时钟和测试信任，不是生产时间同步、持久信任管理或模型接入。

每轮完成真实 DNS、精确 TLS 回显及证书拒绝后，诊断请求
`TLS_STALL_CONFIG_WAIT`。主机只让下一条连接停在 TLS 握手之前，再发送
四字节 `P4F1` 确认；固件验证 10 秒握手超时及新连接回显恢复。
随后在已认证 TLS 流发送 `P4FD`，受控服务端主动关闭 TLS 会话；固件
要求观测 EOF，并再次以新连接精确回显。此故障注入不更改 AP、防火墙
或其他网络设备，不等同于无线链路掉线。栈哨兵及每轮末尾的栈使用分析
仅用于此诊断，不为连接间增加延迟，也不以盲目扩大栈替代故障定位。

结束时关闭本机服务并删除临时私钥；脱敏日志和结果 JSON 保留为私有
证据。P4 清除 CA/端点缓冲及临时证书标签；串口异常或设备崩溃不能保证
设备端清理已执行。判据与限制见[测试指南](testing.md#c5-扫描与网络验收)。

### 硬件看门狗与启动失败

P4 板描述通过 `chosen zephyr,watchdog` 选择 `wdt0`，两个框架配置均启用
`CONFIG_TS_WDT_REQUIRE_HW`，不允许缺少硬件节点时静默退回纯软件保护。
硬件未就绪、安装超时失败或启动失败均返回错误并进入既有启动故障路径；
通道启动重试复用已初始化的 task watchdog，不重复初始化活动定时器。

task watchdog 通道为 5000 ms，后备配置为 `MIN_TIMEOUT=5000` 加
`HW_FALLBACK_DELAY=5000`；参数之和不是已经测量的物理复位时延。Espressif
驱动有中断和复位两阶段，真实时序、停喂复位、调试/睡眠行为须在 H0 实测，
不能从编译通过推断。看门狗本身未能启动时，框架保持故障停机，不能保证
由一个未启动的后备自动复位；人工恢复步骤也属于 H0。

原 10.1 寸设备仅有屏幕、喇叭等交互外设，没有运动机构、外部受控供电或
专用急停；新主力板的实际输出须重新确认。P02 不虚构这些通道或接线；
交互外设在 P04 独立验证，正式产品接入和复杂能力由 P06 承接。其他设备仍须按
[安全不变量](safety.md)声明输出三态、权限、供电预算及适用急停路径，
不能把当前设备没有机构当作通用框架的安全豁免。

### 独立 flash 存储验证配置

在同一准备环境下运行 `make firmware-p4-store-build`，输出进入独立的
`build/firmware-p4-store/`；RAM 基线不变。该配置复用现有 flash 后端，
增加审计恢复启动步骤，不开启测试写入钩子、KV、信任更新或网络部署。

存储验证 overlay 仅拆分默认 `image-1` 预留区，其他分区的地址和尺寸不变：

| 分区 | 起始地址 | 尺寸 |
| --- | --- | --- |
| `ts-prov` | `0x7e0000` | 4 KiB |
| `ts-meta` | `0x7e1000` | 8 KiB |
| `ts-slot-a` | `0x7e3000` | 32 KiB |
| `ts-slot-b` | `0x7eb000` | 32 KiB |
| `ts-noinit` | `0x7f3000` | 4 KiB |
| `ts-audit` | `0x7f4000` | 8 KiB |
| `fw-b` 预留 | `0x7f6000` | `0x7aa000` 字节，至 `0xfa0000` |

`fw-b` 不是已接通的第二固件 OTA 槽。该配置的开发已批准，**实际烧录仍须
另行确认备份、现有分区占用及恢复方式**；不能覆盖现有 `image-1` 数据或
声称真机掉电恢复已通过。存储功能先运行现有 RAM/sim-flash 测试，物理
flash、App 重启恢复和审计仍在 H0 验收。

## 代码与合并

- C 遵循 [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html)；
  格式只是其中一部分，锁、分配、错误路径和数据布局仍须审查。
- `make style BASE=<commit>` 检查指定基线之后更改的第一方 C/H。先运行固件
  setup；`import-baseline` 是风格债务边界，不代表迁入代码全部合规。
- checkpatch 仅对 Zephyr API 排除 `PREFER_KERNEL_TYPES` 与
  `PRINTK_WITHOUT_KERN_LEVEL`：保留 Zephyr 的标准整数类型和 `printk`，不引入
  不存在的 Linux `u32`/`KERN_*` 接口；这两项也列在固定 Zephyr 的
  `.checkpatch.conf`。其他格式检查仍严格执行，不整体继承上游豁免。
- 性能优先服从正确性与硬件安全；性能结论须记录目标、配置、负载及前后数据。
- `agent/` 是独立子模块：先提交 Forge，再更新 Entelea 指针。其环境和测试
  见 [Forge 开发文档](../agent/docs/development.md)。
- Galatea 产品开发在本仓库推进，原研究仓库不是构建依赖；算法或合同迁入
  固定来源、保留许可并重新验证，边界见 [Galatea 产品与研究边界](galatea.md)。
- 安全、信任、公开合同、CI 和贡献者指令变更需要 owner 审查。`CODEOWNERS`
  本身不强制审查；必须在 GitHub 分支规则中配置保护。
- 更改公开行为时更新其权威文档和回归测试。不要提交密钥、运行时工作区、
  构建缓存或会话报告。

测试分类及未覆盖范围见[测试指南](testing.md)；依赖版本和历史来源见
[迁移出处](provenance.md)。
