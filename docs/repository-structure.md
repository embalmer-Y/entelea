# 仓库目录说明

本文是 Entelea 主仓库的目录导航，按用途说明受版本管理的目录；文件和目录的
实际内容以当前 Git 版本为准。[架构与实现边界](architecture.md)说明模块的
职责和完成状态，[开发指南](development.md)及[测试指南](testing.md)说明如何
使用它们。这里的目录名称不代表对应功能已经在目标板验证。

2026-11-06 已规划 P05 对整个项目目录、分层、可插拔模块、代码规范、
debug mode 和集中日志进行重整，详见[阶段调整](requirements.md#2026-11-06-开发阶段调整)。
重整尚未实施；本文仍说明当前结构，不提前把计划目录或日志模块列为已存在。

## 顶层目录

| 路径 | 用途 |
| --- | --- |
| [firmware/](../firmware/) | Zephyr 设备应用、板卡定义、框架模块和固件测试。 |
| [lib/](../lib/) | 可移植的项目基础库，目前包含 `affect/`。 |
| [lib/affect/](../lib/affect/) | 从 Kaleido 迁入的可移植驱力、情感和点火算法及主机测试；尚未接入固件。 |
| [agent/](../agent/) | Entelea Forge 的 Git 子模块，负责 App 工程开发、验证与交付；它是独立仓库，不是设备常驻认知框架。 |
| [tools/](../tools/) | 面向设备和开发过程的辅助工具，目前包含 `board/`。 |
| [tools/board/](../tools/board/) | 面向真实板卡的部署、通信、观测和验收辅助脚本；[受控网络探针](../tools/board/p4_net_probe.py)显式授权后运行 P4/C5 DNS/TLS 及传输故障测试，不烧录。 |
| [scripts/](../scripts/) | 仓库卫生、C 风格、隔离环境、native/P4 构建与测试脚本；[P4 镜像检查器](../scripts/prepare_p4_image.py)补齐 Simple Boot 结束标记，不操作串口。 |
| [patches/](../patches/) | 显式保存的外部项目补丁；是否需要及如何使用应以构建过程和补丁内容为准。 |
| [docs/](./) | 随代码版本维护的架构、开发、测试、安全和来源文档；入口是[文档索引](index.md)。 |
| [LICENSES/](../LICENSES/) | 主许可证之外需要保留的许可声明。 |
| [.github/](../.github/) | GitHub 协作配置：`CODEOWNERS` 和 [workflows/](../.github/workflows/) 下的 CI 工作流。 |
| [.vscode/](../.vscode/) | 随仓库维护的 VS Code 工作区设置和扩展推荐。 |

根目录的 `Makefile`、`pyproject.toml` 和 `uv.lock` 分别提供开发入口、
Python 依赖声明和锁定版本；`AGENTS.md` 保存贡献者及 AI 工具的仓库指引。
Forge 的目录由其独立版本维护，详见 [Forge 的目录入口](../agent/README.md)
和[文档索引](../agent/docs/index.md)。子模块未初始化时这些链接不可用。

## 固件

| 路径 | 用途 |
| --- | --- |
| [firmware/app/](../firmware/app/) | Zephyr 固件应用入口、构建定义和默认配置；[src/](../firmware/app/src/) 为入口源码，[boards/](../firmware/app/boards/) 为应用使用的板级配置和 overlay。 |
| [firmware/boards/](../firmware/boards/) | 仓库维护的 Zephyr 板卡定义，按厂商目录组织。 |
| [firmware/boards/osptek/esp32p4c5_core_board/](../firmware/boards/osptek/esp32p4c5_core_board/) | 仓库维护的 OSPTEK ESP32-P4/C5 板卡定义；`doc/` 是该板的说明。 |
| [firmware/module/](../firmware/module/) | 项目维护的 Zephyr 模块，目前包含 `tessera/`。 |
| [firmware/module/tessera/](../firmware/module/tessera/) | 迁入的 Zephyr 框架模块；保留 `tessera` 和 `ts_*` 兼容命名。 |
| [firmware/module/tessera/include/](../firmware/module/tessera/include/) | 模块对外头文件；[ts/](../firmware/module/tessera/include/ts/) 保存 C 接口。 |
| [firmware/module/tessera/src/](../firmware/module/tessera/src/) | 框架各功能组件的实现，见下表。 |
| [firmware/module/tessera/zephyr/](../firmware/module/tessera/zephyr/) | Zephyr 模块发现和集成元数据。 |
| [firmware/module/tessera/ext/](../firmware/module/tessera/ext/) | 模块关联的外部组件补丁。 |
| [firmware/tests/](../firmware/tests/) | Twister 自动套件、主机检查、网络实验和手动板级工程，见下文。 |

`src/` 下的功能目录：

| 目录 | 用途 |
| --- | --- |
| [appmgr/](../firmware/module/tessera/src/appmgr/) | Wasm App 的包、验签、槽位及运行时管理；`crypto/` 包含相关密码实现与来源说明。 |
| [core/](../firmware/module/tessera/src/core/) | 启动、事件、时间和看门狗等核心机制。 |
| [hal/](../firmware/module/tessera/src/hal/) | 硬件访问接口、权限裁决及音视频、输入和文件等适配。 |
| [interact/](../firmware/module/tessera/src/interact/) | 本地交互操作，如安装确认、仲裁和急停清除。 |
| [net/](../firmware/module/tessera/src/net/) | 网络会话、命令、发布队列、链路监测及 Zenoh 接入。 |
| [periph/](../firmware/module/tessera/src/periph/) | 外设描述、注册及热插拔处理。 |
| [power/](../firmware/module/tessera/src/power/) | 电源槽和预算管理。 |
| [safety/](../firmware/module/tessera/src/safety/) | 输出通道、安全提交、强制安全状态和驱动派发。 |
| [store/](../firmware/module/tessera/src/store/) | 配置、元数据、App 槽、审计及信任信息等存储。 |

## 固件测试

有 `testcase.yaml` 的工程按其配置由 Twister 发现；工程内的 `src/` 通常是
测试源码，`prj.conf` 和 `CMakeLists.txt` 是测试配置及构建入口。个别测试
还有用于生成或构建测试 Wasm 的目录，例如 [app/appw/](../firmware/tests/app/appw/)、
[threads/thrdw/](../firmware/tests/threads/thrdw/)、[wamr/app/](../firmware/tests/wamr/app/)
和 [wamrdemo/app/](../firmware/tests/wamrdemo/app/)。

| 路径（均在 `firmware/tests/` 下） | 用途 |
| --- | --- |
| [app/](../firmware/tests/app/) | App 运行时生命周期、权限和交互链路回归。 |
| [appmgr/](../firmware/tests/appmgr/) | 包安装、验签、槽位、回滚及隔离回归。 |
| [audio/](../firmware/tests/audio/) | 音频 HAL 真实代码与 PDM/ES8311 后端替身的帧、错误及权限回归。 |
| [camera/](../firmware/tests/camera/) | 摄像头 HAL 的帧、重新入队和失败恢复回归；驱动使用替身。 |
| [conc/](../firmware/tests/conc/) | 并发、锁和状态不变量回归。 |
| [core/](../firmware/tests/core/) | 启动、时间、事件及看门狗逻辑回归。 |
| [hal/](../firmware/tests/hal/) | 硬件抽象接口及权限解析回归。 |
| [interact/](../firmware/tests/interact/) | 本地交互核心逻辑回归。 |
| [net/](../firmware/tests/net/) | 网络状态、发布队列和断链处理回归。 |
| [periph/](../firmware/tests/periph/) | 外设注册、输入和热插拔回归。 |
| [power/](../firmware/tests/power/) | 电源预算、限流和电源槽回归。 |
| [replay/](../firmware/tests/replay/) | 确定性重放相关测试。 |
| [safety/](../firmware/tests/safety/) | 输出安全和强制安全状态回归。 |
| [scan/](../firmware/tests/scan/) | 无凭据扫描诊断控制流、事件超时和失败后迟到事件回归；Wi-Fi 驱动为测试替身，不替代 C5 真机。 |
| [sd_diagnostic/](../firmware/tests/sd_diagnostic/) | P4 SD 诊断真实代码与文件系统/磁盘替身的成功和故障控制流回归。 |
| [smoke/](../firmware/tests/smoke/) | Zephyr 模块接线冒烟测试。 |
| [store/](../firmware/tests/store/) | 配置、元数据、槽位和存储恢复回归。 |
| [threads/](../firmware/tests/threads/) | Wasm App 多线程相关回归。 |
| [wamr/](../firmware/tests/wamr/) | WAMR 运行时集成回归。 |
| [wamrdemo/](../firmware/tests/wamrdemo/) | WAMR 示例及运行链路回归。 |
| [netlab/](../firmware/tests/netlab/) | 需要额外网络配置的仿真部署实验，不是普通 Twister 自动套件。 |
| [pytest/](../firmware/tests/pytest/) | 主机侧 Python 回归，包括仓库卫生、文档链接、P4 镜像终止检查以及受控 DNS/TLS 服务；不代替设备端 C 和真机验证。 |
| [safety_check/](../firmware/tests/safety_check/) | 固件安全合同的机械检查脚本。 |
| [board/](../firmware/tests/board/) | 需要对应设备或专用环境的手动板级实验，见下表。 |

### 板级实验

各工程有自己的构建配置及适用设备，存在时的 `src/` 和 `boards/` 分别存放
工程源码及板级配置。下表只是导航，**不是已验证清单或运行许可**；运行前
应阅读[测试指南](testing.md)和[板级工具说明](../tools/board/README.md)，
核对源码、设备、接线、恢复方案并取得授权。

| 目录（均在 `firmware/tests/board/` 下） | 主要用途 |
| --- | --- |
| [atkbench/](../firmware/tests/board/atkbench/) | ATK DNESP32-S3B bring-up 与部署链实验。 |
| [auditbench/](../firmware/tests/board/auditbench/) | 审计记录的 flash 持久化实验。 |
| [avbench/](../firmware/tests/board/avbench/) | 视频帧网络分片传输测量。 |
| [avdemo/](../firmware/tests/board/avdemo/) | 视频采集与 App 传输链路示例。 |
| [boardbench/](../firmware/tests/board/boardbench/) | 板级时序与效率基准。 |
| [cambench/](../firmware/tests/board/cambench/) | 摄像头初始化与采集实验。 |
| [deploybench/](../firmware/tests/board/deploybench/) | Forge 到设备的部署链路实验。 |
| [dsdbench/](../firmware/tests/board/dsdbench/) | App 的 SD 卡文件访问链路示例。 |
| [estopbench/](../firmware/tests/board/estopbench/) | 急停输入及安全路径实验。 |
| [h7bench/](../firmware/tests/board/h7bench/) | H7 板上的配置和 App 等 flash 持久化实验。 |
| [inputdemo/](../firmware/tests/board/inputdemo/) | 输入事件与 ADC、PWM 控制示例。 |
| [linkdemo/](../firmware/tests/board/linkdemo/) | 输入控制、断链安全状态与恢复示例。 |
| [metabench/](../firmware/tests/board/metabench/) | 元数据轮转写入与复位恢复实验。 |
| [netbench/](../firmware/tests/board/netbench/) | Wi-Fi、Zenoh 命令通信及延迟实验。 |
| [otabench/](../firmware/tests/board/otabench/) | MCUboot 和 MCUmgr 固件 OTA 实验。 |
| [p4bench/](../firmware/tests/board/p4bench/) | ESP32-P4 上的 flash 持久化实验。 |
| [p4psram/](../firmware/tests/board/p4psram/) | ESP32-P4 上的 PSRAM 和 App 内存实验。 |
| [p4sdbench/](../firmware/tests/board/p4sdbench/) | P4 SD 独占文件读写、清理及同卡重新挂载诊断；实际运行会写入测试卡。 |
| [p4wifibench/](../firmware/tests/board/p4wifibench/) | ESP32-P4 与 Wi-Fi 伴芯的联网实验；独立无凭据扫描及运行时凭据/CA/测试时间配置的 DNS/TLS/故障恢复诊断。 |
| [pdmbench/](../firmware/tests/board/pdmbench/) | PDM 音频采集驱动实验。 |
| [pdmdemo/](../firmware/tests/board/pdmdemo/) | PDM 音频 App 采集与传输示例。 |
| [periphbench/](../firmware/tests/board/periphbench/) | PWM、ADC 等外设后端实验。 |
| [persistbench/](../firmware/tests/board/persistbench/) | 配置、App 和元数据的 flash 持久化实验。 |
| [psrambench/](../firmware/tests/board/psrambench/) | PSRAM 与 App 内存实验。 |
| [sdbench/](../firmware/tests/board/sdbench/) | SD 卡挂载和读写实验。 |
| [thrdemo/](../firmware/tests/board/thrdemo/) | Wasm App 多线程示例。 |
| [wdtbench/](../firmware/tests/board/wdtbench/) | 看门狗及 App 健康故障处理实验。 |

## 情感算法库

| 路径 | 用途 |
| --- | --- |
| [lib/affect/kaleido_drive/](../lib/affect/kaleido_drive/) | 社交、刺激和休憩驱力的状态计算。 |
| [lib/affect/kaleido_emotion/](../lib/affect/kaleido_emotion/) | 情感状态及相关输出计算。 |
| [lib/affect/kaleido_ignition/](../lib/affect/kaleido_ignition/) | 主动触发的评分、门控和结算逻辑。 |
| [lib/affect/tests/](../lib/affect/tests/) | 主机测试的公共入口和套件声明。 |
| [lib/affect/tests/reference/](../lib/affect/tests/reference/) | 固定 Galatea 来源的测试参考算子和 C 夹具生成入口；不是第二套运行时情感引擎。 |

三个算法目录各有 `include/`、`src/` 和 `test/`，分别存放接口、实现和测试。
它们目前是可独立测试的内核，不应理解为已经具备固件内可插拔生命周期。

## 本地生成目录

以下目录由开发环境生成或下载，通常被 `.gitignore` 排除，并非随仓库交付的
源码；清理前应确认其中是否有需要保留的本地数据：

| 路径 | 用途 |
| --- | --- |
| `.venv/` | 主仓库 Python 虚拟环境；Forge 在自己的工作区中维护独立环境。 |
| `.deps/` | 本地固定 Zephyr、WAMR 源码，以及 P4 可选准备的 Espressif HAL、FatFS 等模块。也可以按开发指南只读指定外部源码。 |
| `build/`、`build-*/`、`twister-out*/` | 构建输出及测试结果，具体位置以实际执行的命令为准。 |
| `__pycache__/`、`.pytest_cache/`、`.ruff_cache/` | Python、pytest 和 Ruff 的本地缓存。 |
