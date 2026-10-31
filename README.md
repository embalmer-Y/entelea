# Entelea

Entelea 是一个基于 Zephyr 的具身 AI 框架：让设备感知环境、通过受保护的
硬件能力行动，并由独立的 [Entelea Forge](https://github.com/embalmer-Y/entelea-forge)
制造和验证新的 App。

## 当前状态

- 已迁移 Zephyr 固件、硬件抽象、安全路径、Wasm App 管理和 TSAP 交付机制。
- Kaleido 的驱动、情感和触发内核可以独立测试，**尚未接入固件**。
- 常驻认知框架、Lua 执行、多 App 通道仲裁和情感生命周期仍在设计中。
- 主机测试不等于特定开发板、执行机构或实时性能已验证。

## 快速开始

需要 Linux x86-64、Python 3.12、`uv`、C 编译器、CMake、Ninja 和支持
wasm32 的 clang。安装与固件构建的完整前置条件见[开发指南](docs/development.md)。

```sh
git clone --recurse-submodules https://github.com/embalmer-Y/entelea.git
cd entelea
uv sync --locked
uv sync --project agent --locked --extra dev
make check
make agent-test
```

这些命令不烧录设备或调用付费模型；首次构建情感测试可能需要网络下载
固定版本的 Unity。成功时测试命令退出码为 0；固件 native_sim 另见
[测试指南](docs/testing.md)。

## 目录与文档

| 路径 | 用途 |
| --- | --- |
| `firmware/` | Zephyr 固件、板级定义、自动回归和手动板级实验 |
| `lib/affect/` | 可移植情感内核与主机测试 |
| `agent/` | 独立版本管理的 Forge 子模块 |
| `tools/board/` | 需要明确授权的板级辅助与验收工具 |
| `scripts/`、`patches/` | 开发检查和显式维护的外部补丁 |
| `docs/` | 随代码版本维护的项目文档 |

从[文档索引](docs/index.md)选择架构、开发、测试、安全及来源说明。
除文件或第三方声明另有说明外采用 Apache-2.0；情感内核保留
[Galatea MIT 许可](LICENSES/Galatea-MIT.txt)。
