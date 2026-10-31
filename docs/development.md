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

## 代码与合并

- C 遵循 [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html)；
  格式只是其中一部分，锁、分配、错误路径和数据布局仍须审查。
- `make style BASE=<commit>` 检查指定基线之后更改的第一方 C/H。先运行固件
  setup；`import-baseline` 是风格债务边界，不代表迁入代码全部合规。
- 性能优先服从正确性与硬件安全；性能结论须记录目标、配置、负载及前后数据。
- `agent/` 是独立子模块：先提交 Forge，再更新 Entelea 指针。其环境和测试
  见 [Forge 开发文档](../agent/docs/development.md)。
- 安全、信任、公开合同、CI 和贡献者指令变更需要 owner 审查。`CODEOWNERS`
  本身不强制审查；必须在 GitHub 分支规则中配置保护。
- 更改公开行为时更新其权威文档和回归测试。不要提交密钥、运行时工作区、
  构建缓存或会话报告。

测试分类及未覆盖范围见[测试指南](testing.md)；依赖版本和历史来源见
[迁移出处](provenance.md)。
