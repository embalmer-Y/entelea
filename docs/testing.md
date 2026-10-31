# 测试与验证范围

本文面向贡献者。环境准备见[开发指南](development.md)；普通验证不刷写设备
或调用付费模型。

| 入口 | 范围 | 限制 |
| --- | --- | --- |
| `make check` | 仓库、Python 回归、安全机械检查和情感内核主机测试 | 不验证板卡 |
| `make firmware-build` | native_sim 固件构建 | 不是交叉编译板级镜像 |
| `make firmware-test` | `firmware/tests/` 下有 `testcase.yaml` 的 Twister 自动套件 | 不自动执行手动板级实验 |
| `make agent-test` | Forge 全量主机测试、跨仓库夹具及 Skill 同步 | 部署与仿真 E2E 按其显式条件启用 |
| `make style BASE=<commit>` | 相对基线新增或修改的第一方 C/H 文件 | 不覆盖导入基线的全部风格债务 |

`firmware/tests/board/` 保存需要对应板卡或其他专用环境的实验工程；每项的
实际适用板、配置和判据以工程内容为准，不能把它们计作 CI 已执行。`netlab/`
用于需额外网络配置的仿真部署；`pytest/` 与 `safety_check/` 为主机检查。
[板级工具说明](../tools/board/README.md)列出运行风险和先决条件。

测试结果应报告实际配置、通过/跳过数和限制。设备安全值、急停效果、网络
失联行为及时间预算需要在目标机构与板卡上独立测量。生成测试根密钥为公开
夹具，不能用于生产设备。
