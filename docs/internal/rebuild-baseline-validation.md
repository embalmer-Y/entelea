# 重建基线验证记录

2026-11-08。此记录随源码基线提交，不表示旧框架已完整验收。
基线 commit 由后续新版本迁移清单固定，避免本文自引用 commit。

## 本轮实际检查

- Git 主仓库既有修改与新增源码/文档纳入基线；Forge 工作区干净，
  gitlink 为 `ca5219e7cf6929749b699d8d82da33cdc4eb463e`。
- 非忽略候选文件检查未发现私钥头、常见 token 或敏感文件名匹配；
  不构成全面保密或漏洞审计。忽略的环境、缓存、私有配置不提交。
- `make check`：
  - 仓库卫生通过。
  - scripts/参考代码 Ruff 通过。
  - Galatea 数值参考通过。
  - affect 构建及 CTest `affect_golden` 1/1 通过。
  - 安全机械检查失败，后续聚合步骤未执行。
- 独立执行 `./.venv/bin/python -m pytest firmware/tests/pytest -q`：
  **243 passed, 1 failed**，失败为
  `test_safety_contract_self.py::test_repo_smoke_green`。

## 已知失败，不能改测试制造绿灯

安全检查只允许保护分发与既有 boardbench 的输出驱动调用，
而独立诊断工程 `firmware/tests/board/p4acoustic/src/main.c` 的三个
`gpio_pin_set` 和 `firmware/tests/board/p4audiopc/src/main.c` 的一个
`gpio_pin_set` 被报告越界。Python 失败复用同一检查，属于同一根因。

本轮不更改诊断或扩大豁免。重建时先将独立硬件诊断和产品执行代码
明确区分；检查范围/许可策略及真实输出保护需单独决定并测试。
不能据此认定真实漏洞或把所有诊断目录加入宽泛白名单。

## 未运行与证据边界

本轮未执行完整 native_sim、Forge 套件、P4 构建、烧录、真实网络/
模型调用或硬件验收。P02/P03/P06、H0/I1/I2/I3 未关闭项以
[需求进度](../requirements.md)和[测试记录](../testing.md)为准。
旧独立硬件通过不继承为新版本产品通过。
