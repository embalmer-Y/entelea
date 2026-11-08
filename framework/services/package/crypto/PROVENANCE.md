# crypto/ 第三方与自研密码件出处（军规 6 登记配套）

## tweetnacl.c / tweetnacl.h

- 出处：TweetNaCl（tweetnacl.cr.yp.to/20140427/tweetnacl.c，20140427 快照，逐字节未改）。
- 许可：Public Domain（作者声明 https://tweetnacl.cr.yp.to/）。
- 用途（DEC-49①B）：**仅 crypto_sign_open（ed25519 验签）**——安装/激活期一次性
  调用（非热路径）；keypair/sign 等函数不引用（链接期 gc 丢弃）。
- 编译：`-w`（上游致密风格代码，本项目警告基线不适用于三方源）。
- 对拍互验：与 Agent 侧 DIY 实现（cryptography/ed25519）四象限互验 = 同一
  双实现纪律（DR-21）的固件侧延伸；test root 密钥对（agent/tests/fixtures/）
  驱动跨实现往返。

## ts_sha256 → 复用 ts-store 实现

- sha256 由 `src/store/sha256.c` 提供（同 API `ts_sha256_init/update/final`，
  M2a 起 slot hash 使用）——DEC-49② 验签管线直接复用（零重复实现；
  首版曾自研副本，链接器多重定义当场拦截后收敛——军规 9 反向例证）。
