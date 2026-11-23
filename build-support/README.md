# 构建与依赖边界

默认只构建可移植公共库和主机测试，无 Zephyr/WAMR/HAL 依赖，
不自动下载依赖、应用补丁、访问设备或调用模型。

```sh
cmake -S . -B build/r01 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/r01
ctest --test-dir build/r01 --output-on-failure
cmake -S . -B build/common-only -G Ninja -DBUILD_TESTING=OFF
cmake --build build/common-only
```

需要ASan/UBSan时另建主机目录并启用 `-DENTELEA_SANITIZERS=ON`，
编译器须有其配套runtime。本机Clang缺ASan库，使用现有GCC验证，
不是禁用检查制造通过。BUILD_TESTING=OFF默认不构建旧void适配库。

## 工作区、Zephyr 构建与 SDK 目录合同

现有 Zephyr 入口为 `tools/build_zephyr.py`，按目标核验固定源码、
模块、SDK/工具链并检查生效配置。实际构建和运行证据见对应验证文档；
构建入口存在不等于任一目标的物理或产品验收通过。

| 类别 | 目录与约束 |
| --- | --- |
| 受管源码 | 仓库忽略的 `.deps/`：`zephyrproject/`（`west init -l` 绑定固定 Zephyr 4.5 提交 41d10b5；模块由 `west update` 按 west.yml 拉取；应用 6 个已批准补丁）、`wamr/`、`esp-hosted-mcu/`、`lua/`、`zephyr-sdk-1.0.1/` |
| 外部源码与产物 | 仓库外 `/home/emb/project/deps/`：ESP-IDF v5.5.4 与 C5 固件产物（路径写入构建脚本与 `c5-firmware.json`） |
| 构建产物 | 统一放本仓库 `build/<batch>-<target>`；可再生，不作证据保存 |
| Python 环境 | 构建：仓库内 `.venv/`（CPython 3.12，`uv venv --seed`；Zephyr 依赖经 `west packages pip` 安装，ruff 随之固定为 0.14.2）。west 另放 `/home/emb/project/envs/west`，构建解释器不得可导入 west（否则 Zephyr 继承整个工作区模块集，`build_zephyr.py` 拒绝）。OpenHands：`/home/emb/project/envs/openhands`，由带哈希锁 `factory/forge/requirements-openhands.lock` 同步 |
| Zephyr SDK | `.deps/zephyr-sdk-1.0.1/`（仓库忽略），`build_zephyr.py --sdk` 设置 `ZEPHYR_SDK_INSTALL_DIR` 与 `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`（官方方式，不注册 CMake 包、不执行 setup、不修改 shell 配置） |

本机 SDK 为 `sdk_version` `1.0.1`，实际 RISC-V GCC 为 `14.3.0`，
编译器位于 `gnu/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gcc`。
这是本机版本核验结果，不批准所有未来板卡组合；
不同主机或目标的变更仍须显式记录，不能凭目录名认定版本。

构建入口在调用 CMake/Twister 前须落实并负测：

1. 显式选择 `ZEPHYR_BASE` 和实际模块目录，核验固定提交、完整批准补丁、
   staged/untracked/mode 差异；现有核验器可复用，不只做 reverse-check。
2. native_sim 显式选择适用 host 工具链；板级目标显式选择
   `ZEPHYR_TOOLCHAIN_VARIANT=zephyr` 和 SDK 路径，不混用 R01 主机编译器。
3. 校验 SDK 实际 `sdk_version`、所需目标编译器的存在/版本和模块组合。
   缺失、版本不符、实验污染或无批准组合时显式失败，不自动下载或回退。
4. 使用新目标的独立构建目录，不消费旧 CMake/Twister 缓存或全局 west
   工作区。若后续引入 west，其 manifest/模块选择也必须显式固定。
5. 记录实际路径、版本、配置与执行证据；源码/SDK 核验通过不等于
   编译通过，交叉编译通过不等于设备运行或物理停止通过。

旧顶层 `zephyrproject` 与历史归档已于 2026-11-12 按所有者决定删除。
旧 `zephyrproject` 含 DMA 实验修改，不符合批准补丁集合，不再使用；
当前 Zephyr 仅来自上文 `.deps/zephyrproject` 的固定提交与 west 模块。
外部 ESP-IDF 与 C5 产物仍在 `/home/emb/project/deps/`（仓库外，路径已写入脚本与清单）。

## 来源与核验

[固定依赖](dependencies.json)涵盖已采用的 Zephyr/Wasm 目标的
Zephyr、WAMR、HAL、nanopb、mbedtls、tf-psa-crypto；west 模块修订与清单
逐项一致（所有者决定以 west.yml 为准并同步核对）。
主机公共库不消费这些依赖；元数据通过不表示固件构建通过。

Forge 工厂已于 2026-11-12 并入 [factory/forge](../factory/forge/)（快照自
ec24f00；历史 bundle 位于 `/home/emb/project/forge-ec24f00.bundle`）。编程引擎为
OpenHands Software Agent SDK 1.60.2，环境位于 `/home/emb/project/envs/openhands`，
由 [带哈希锁](../factory/forge/requirements-openhands.lock) 同步。
它是工厂的 Python 工程依赖，不是 Zephyr SDK 或 MCU 公共库依赖，不加入固件三项 pin。
隔离与产品验收仍未闭合；边界见
[工厂引擎决策](../docs/rebuild-plan.md#71-工厂编程引擎已确认决策)。

```sh
python3 tools/check_dependencies.py
python3 tools/check_dependencies.py --dependency zephyr --checkout .deps/zephyrproject/zephyr
```

完整diff、版本、staged/untracked和mode均核验，**只读、不修补**。
Zephyr checkout须具备全部六个批准补丁，不能仅reverse-check其中一个；
原始干净Zephyr对有补丁目标会明确失败，不自动下载/应用。
WAMR/HAL须为干净固定版本。实验GDMA/CPLL补丁未迁入。
补丁保持原字节和基线摘要；顺序沿用旧准备脚本，并与唯一资产台账核对。
原diff载荷中的上下文前缀空白不做源码格式化；`.editorconfig` 对 `*.patch`
关闭行尾空白裁剪，防止编辑器改写被钉死的字节；内容完整性仍强制按基线SHA256
核验。空白样式检查随其实现批次恢复时，同样必须精确排除 patches/*.patch。

第三方来源/许可沿用固定旧provenance，不改变批准范围：

| 来源 | 固定版本标识 | 许可/范围 |
| --- | --- | --- |
| Zephyr upstream | [固定commit](https://github.com/zephyrproject-rtos/zephyr/commit/41d10b59730406bbab4ca9f1fbbe9e96aafb4966) | Apache-2.0；迁入仅六个批准补丁，未复制上游树 |
| WAMR upstream | [固定commit](https://github.com/bytecodealliance/wasm-micro-runtime/commit/25bd7eb63e828e4bd242cc9b38d260b4b31c6605) | 按该版本LICENSE核验；本轮仅锁定元数据，未复制代码 |
| HAL upstream | [固定commit](https://github.com/zephyrproject-rtos/hal_espressif/commit/cc81db00233af6020f2fea9fbb0d0098fb3d0d86) | 分文件许可按旧provenance/上游LICENSE；本轮未复制代码 |
| 共享SHA/CBOR | 资产A-SHA/A-CBOR，旧主仓库基线 | Apache-2.0；保留SPDX，变化见[公共合同](../framework/common/contract.md) |

## 产品/诊断检查边界

[源分类](source-policy.json)是CMake生产/兼容/测试/诊断源表。
不按目录宽泛豁免：每个C源必须出现一次；诊断必须给出具体目标与具体源，
只能在tests/diagnostics下，独立EXCLUDE_FROM_ALL，不能作为common/compat输入。Zephyr 框架及平台/测试源也有明确分类；
新增源后需更新分类并重新配置。公共头/机制限制依赖、直接时间源和
输出调用；保护驱动入口在 `protected_output_sources` 中精确列出，
不能把其他旧 driver 路径当隐式白名单。
这是正则/显式来源机械约束，不是完整C调用图、漏洞审计或物理安全证明；
不替代旧L5的全部规则，其暂不适用规则在对应实现批次恢复。

## 验证入口

CTest包括C合同、架构、800项固定来源、Python治理负测、
依赖元数据与旧C源码对拍。legacy_equivalence从固定Git对象读取，
在临时目录仅替换私有include、编译时重命名原符号，与迁入真实ts符号及
Python hashlib/独立CBOR字节向量对比；不修改归档、不由新算法生成oracle。
R01 本身不覆盖完整 TSAP/COSE、运行时隔离、Lua VM 或物理输出；
后续各批证据与当前未测边界见 [RF1 复核](../docs/rf1-validation.md)。
