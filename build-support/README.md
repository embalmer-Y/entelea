# R01 构建与依赖边界

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

## 来源与核验

[固定依赖](dependencies.json)目前只涵盖未来首个Zephyr/Wasm目标的
Zephyr、WAMR、HAL，不宣称完整SDK/所有可选模块可重现。
其他模块在需要时从固定旧 `docs/provenance.md` / 锁文件选择并重新核验。
主机公共库不消费这三个依赖；元数据通过不表示固件构建通过。

```sh
python3 tools/check_dependencies.py
python3 tools/check_dependencies.py --dependency zephyr --checkout ../entelea-legacy/.deps/zephyr
```

完整diff、版本、staged/untracked和mode均核验，**只读、不修补**。
Zephyr checkout须具备全部六个批准补丁，不能仅reverse-check其中一个；
原始干净Zephyr对有补丁目标会明确失败，不自动下载/应用。
WAMR/HAL须为干净固定版本。实验GDMA/CPLL补丁未迁入。
补丁保持原字节和基线摘要；顺序沿用旧准备脚本，并与唯一资产台账核对。
原diff载荷中的上下文前缀空白不做源码格式化；空白样式检查精确排除
patches/*.patch，内容完整性仍强制按基线SHA256核验。

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
只能在tests/diagnostics下，独立EXCLUDE_FROM_ALL，不能作为common/compat输入。
本轮没有实际诊断目标，不复制旧硬件工程或为其建立空目录。
新增源后需重新配置；配置及CTest均执行架构检查。
公共头/机制限制依赖、直接时间源和输出调用；R01保护入口列表为空，
因此任何非诊断源直接输出都失败。R02必须单独评审保护实现，
不能把旧driver_dispatch路径当已迁入的隐式白名单。
这是正则/显式来源机械约束，不是完整C调用图、漏洞审计或物理安全证明；
不替代旧L5的全部规则，其暂不适用规则在对应实现批次恢复。

## 验证入口

CTest包括C合同、架构、800项固定来源、Python治理负测、
依赖元数据与旧C源码对拍。legacy_equivalence从固定Git对象读取，
在临时目录仅替换私有include、编译时重命名原符号，与迁入真实ts符号及
Python hashlib/独立CBOR字节向量对比；不修改归档、不由新算法生成oracle。
完整TSAP/COSE验签、运行时隔离、Lua有界停止和实际安全输出均未在R01验证。
