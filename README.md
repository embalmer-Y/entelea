# Entelea

面向 Zephyr MCU 与原生 Linux ARM64/x86 的智能硬件和模拟智能框架。
已完成 **R01 公共合同与构建底座**：时间/截止时间、独立CBOR/SHA、
所迁旧子集适配和主机验证。尚无新运行时、驱动或产品实现。
本轮在R01验收后停止，R02未启动。结果见[验证记录](docs/r01-validation.md)。

- [重建规划](docs/rebuild-plan.md)：顺序、边界、决策闸和验收。
- [唯一资产台账](docs/migration-assets.json)：固定来源、模块映射、迁移与验证状态。
- [接手 prompt](docs/agent-handoff-prompt.md)：新 Agent 首轮核对、协作边界和验证入口。
- 旧源码在相邻 `entelea-legacy`，主基线
  `e54047b51519fa449dfe4a338af8d126bed8fac4`；Forge 单独固定版本。
- 新目录是 linked worktree；旧目录包含共享 Git 对象库，**不能删除**。
- 本地 `rebuild` 分支；远端和默认分支未改，未推送。

已认可 D01–D12 的建议方向，D03 调整为首期设计覆盖 Lua；
具体未决边界仍按[评审记录](docs/rebuild-plan.md#7-设计确认清单与不得擅定的阈值)逐批冻结。
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
