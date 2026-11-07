# Entelea

面向 Zephyr MCU 与原生 Linux ARM64/x86 的智能硬件和模拟智能框架。
当前是干净重建的**治理骨架**，尚无新运行时或产品实现。

- [重建规划](docs/rebuild-plan.md)：顺序、边界、决策闸和验收。
- [唯一资产台账](docs/migration-assets.json)：固定来源、模块映射、迁移与验证状态。
- 旧源码在相邻 `entelea-legacy`，主基线
  `e54047b51519fa449dfe4a338af8d126bed8fac4`；Forge 单独固定版本。
- 新目录是 linked worktree；旧目录包含共享 Git 对象库，**不能删除**。
- 本地 `rebuild` 分支；远端和默认分支未改，未推送。

开始下一批前先按规划 review D01–D05，不将内部设计建议当成已批准范围。
默认只读取当前批次所需上下文；没有业务代码不意味着框架验收通过。

```sh
python3 tools/check_migration.py --sources
python3 -m unittest discover -s tools/tests -v
```

上述命令只验证迁移元数据、固定来源及校验器，不构建固件、不调用云或硬件。
旧基线检查并非全绿：Python 为243 passed、1 failed（L5机械检查同一根因）；
具体边界与后续处理见规划，不把它继承为新产品完成。

许可沿用 [Apache-2.0](LICENSE)；迁入第三方资产须保留各自许可与来源。
