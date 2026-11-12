# Entelea

面向 Zephyr MCU 与原生 Linux ARM64/x86 的智能硬件和模拟智能框架。
R00–R12 有历史交付记录，包含受限运行时、保护、存储、辅助智能、
工厂和部分板级诊断；历史批次通过不等于最初规划的全部能力完成。
**RF1 审查整改仍有未闭合项**：试用确认权限已与运行时授权取交集，稳定集准入（D-E）
已实施；持久经历和完整产品验收等仍未完成。
R13 仍待完整框架/产品验收。逐项范围、已验证内容和阻塞见
[RF1 复核](docs/rf1-validation.md)。

- [重建规划](docs/rebuild-plan.md)：顺序、边界、决策闸和验收。
- [唯一资产台账](docs/migration-assets.json)：固定来源、模块映射、迁移与验证状态。
- [框架 Wiki](docs/wiki/README.md)：模块职责、调用链、平台差异和限制。
- 旧源码在相邻 `entelea-legacy`，主基线
  `e54047b51519fa449dfe4a338af8d126bed8fac4`；Forge 单独固定版本。
- 新目录是 linked worktree；旧目录包含共享 Git 对象库，**不能删除**。
- 本地 `rebuild` 分支；远端历史与当前状态应以 Git 实际查询为准。

已认可 D01–D12 的建议方向，D03 调整为首期设计覆盖 Lua；
具体未决边界仍按[评审记录](docs/rebuild-plan.md#7-设计确认清单与不得擅定的阈值)逐批冻结。
Forge 保留独立工程仓库，编程引擎已确定采用 **OpenHands Software Agent SDK**，
不继续自研通用创作循环；独立工作区已有 SDK 接入试验，隔离和版本化等
产品保障尚未闭合。
职责与迁移边界见[工厂引擎决策](docs/rebuild-plan.md#71-工厂编程引擎已确认决策)。
默认只读取当前批次所需上下文；公共机制通过不意味着设备/产品验收通过。
公共机制的边界见[合同](framework/common/contract.md)，
构建/依赖/诊断检查见[构建说明](build-support/README.md)。

```sh
cmake -S . -B build/r01 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/r01
ctest --test-dir build/r01 --output-on-failure
python3 tools/check_migration.py --sources
python3 -m unittest discover -s tools/tests -v
```

上述命令编译公共C机制并验证合同/迁移元数据；不构建固件、不调用云或硬件。
旧基线检查并非全绿：Python 为243 passed、1 failed（L5机械检查同一根因）；
具体边界与后续处理见规划，不把它继承为新产品完成。

许可沿用 [Apache-2.0](LICENSE)；迁入第三方资产须保留各自许可与来源。
