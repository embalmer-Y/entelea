# 迁移来源与依赖

## 导入的代码

- [Tessera 的固定提交](https://github.com/embalmer-Y/Tessera/tree/64a2cb7b2cb010be1951bf4a25e153f41519efad)：
  固件、板级工具、许可及属性；其 Agent 子树独立导入 Forge。
- Kaleido 提交 `3fbc72266b25e4eee1a0667b7d7af4f9e7157ee9`：提取
  `kaleido_{drive,emotion,ignition}` 及原有黄金测试至 `lib/affect/`。
- Galatea 衍生情感代码保留 MIT 许可及署名；TweetNaCl 保留自身来源声明。

`import-baseline` 标签记录适配前的导入。源代码中的历史 DEC/Q/HLD/LLD
引用应在上面的 Tessera 提交中查找，不代表当前需求。旧计划、研究和会话
记录没有复制到当前文档；原仓库保持不变。

## 外部依赖

| 依赖 | 固定版本或提交 |
| --- | --- |
| Zephyr v4.5.0 | `41d10b59730406bbab4ca9f1fbbe9e96aafb4966` |
| WAMR 2.4.5 | `25bd7eb63e828e4bd242cc9b38d260b4b31c6605` |
| zenoh-pico 1.10.1 | `e1ab223a28aaebb5dec1e70d98eab152332f777a` |
| Unity 2.6.1，主机测试 | `cbcd08fa7de711053a3deec6339ee89cad5d2697` |

`patches/zephyr-esp32-gdma.patch` 保存原 Zephyr 工作区中未提交的 ESP32 DMA
修改，并非上游代码；仅在对应板级验证时审查和应用，native_sim 不覆盖它。
zenoh TLS 补丁位于 `firmware/module/tessera/ext/zenoh-pico-tls.patch`。

## 有意排除与适配

没有迁入本地凭据、配置、工作区、构建缓存、会话日志及 ESP-IDF 对话/UI
外壳。公开测试签名材料仅作夹具，不得配置到生产设备。

旧环境安装器由隔离源码准备脚本替换，其测试已针对新脚本改写并保留 TLS
补丁完整性检查。Forge 的 gateway 子进程测试使用临时工作区。情感 ignition
空参数测试改为初始化结果并逐项检查不变性；内核行为未改。
