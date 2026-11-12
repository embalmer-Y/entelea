# Entelea 框架 Wiki

本 Wiki 介绍当前代码的结构和调用方式，不是第二份进度台账，也不把
诊断、模拟器结果或历史真机记录当成当前产品验收。完整状态与未完成项见
[RF1 复核](../rf1-validation.md)；迁移状态以
[唯一资产台账](../migration-assets.json)为准。

## 按需阅读

1. [整体架构与平台](architecture.md)：分层、装配、数据流和平台差异。
2. [监督与执行](runtime.md)：事件、WDT、安全、身份、资源和任务。
3. [框架服务](services.md)：存储、配置、网络、模型、媒体、记忆与情感。
4. [包、试用与工厂](delivery.md)：签名包、Wasm、用户确认、Forge 及缺口。

每页链接已有模块合同；修改 API 或行为时先核对对应合同，不复制一份新的
规格。历史验证文档仅证明当时记录的配置和执行范围。

## 代码与命名

- 项目自有代码使用 `entelea_` 前缀、按职责命名的目录和就近合同；
  C/H 采用 tab 缩进，Python 使用 4 空格，文本使用 UTF-8 和 LF，
  以仓库 [.editorconfig](../../.editorconfig) 为准。
- 公共头在 `framework/include/entelea`；执行机制、服务和平台装配分别
  位于 `framework/execution`、`framework/services` 和 `platform`。
  测试/诊断必须在 [source-policy.json](../../build-support/source-policy.json)
  中精确分类。
- `D12` 是重建规划中的第 12 项设计决策编号，不是行业标准或独立硬件。
  当前 `d12` API 实际承载部分运行预算和复位风暴保护；编号作为长期
  模块名并不理想，但重命名涉及公共 API 和调用方，应在兼容方案确认后
  进行，不能为统一风格直接改名。
- 保留第三方来源、许可和固定补丁字节；不因格式审计改写上游代码。
  兼容符号、TSAP 格式和持久记录必须经过迁移/兼容评估才能改变。
