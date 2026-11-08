# R01 公共合同与构建底座验证

## 交付范围

本轮按用户确认只完成R01，R02未开始。采用建议目录、新Entelea公共命名、
显式ts_*旧子集适配；不改旧TSAP/COSE字节格式。
产物：独立时间/截止时间、CBOR/SHA256、来源/可信身份/App接入表示、
主机CMake/CTest、六个原字节批准补丁与三个依赖元数据、只读核验、
产品/诊断精确源分类及负测。

Lua在公共App合同中覆盖，未安装解释器/新依赖，未证明VM预算/停止，
不宣称正式运行支持。完整包schema/验签与Wasm执行仍归R05。
旧同步事件/监督/保护未迁入，物理安全与平台后端验收仍归R02。

## 验证方法及结果

- GCC 15.2 C11主机Debug：配置/编译/6项CTest通过。
  CTest覆盖common_contracts、architecture、migration_sources、
  governance_tests、dependency_manifest、legacy_equivalence。
- GCC配套ASan/UBSan：同一主机合同和6项CTest通过。
  本机Clang18普通编译成功，ASan链接因缺少配套clang_rt库失败；
  未安装/修改共享工具，改用已有GCC runtime完成sanitizer验证。
- BUILD_TESTING=OFF Release：只构建entelea_common静态库通过，
  未构建compat，无Zephyr/WAMR/HAL/网络/驱动链接。
- 新公共头和所迁旧头作为C++11编译通过；只证明本机头兼容。
- 固定旧C源码在隔离临时目录编译对拍：只替换私有include并重命名符号，
  合法整数/bool/容器/字符串编码、解码值、SHA一次性/流式对拍通过。
  SHA另对Python hashlib，包括百万a与块/填充边界；CBOR使用独立字节向量。
  非法旧void API由独立子进程确认明确报错/SIGABRT，禁生成core。
- C合同覆盖输入/容量/有符号范围/递归和短读、输出/游标失败不变，
  跨时基/时钟回退/无绑定/截止时间溢出、旧类型布局/预处理错误值、
  Wasm/Lua不同预算表示和取消未知效果。
- Python治理48项通过，包含既有23项、23项R01依赖/架构负测及2项
  新-only资产实现commit/目标验证负测：
  版本/完整diff/额外staged/untracked/候选补丁/摘要漂移，
  输出旁路/私有依赖/直接时间/未分类源/诊断混入等拒绝。
- 三个实际旧依赖工作区的只读检查通过：Zephyr完整六补丁预期diff，
  WAMR/HAL固定干净版本；不修改旧依赖、不复制缓存。
- 固定800项源对象/摘要保持一致；源ID不变，迁移状态仅标实际源项。
  登记13个实际提取/适配条目（含6补丁）、一个新-only合同资产；
  共115个资产组。CBOR/SHA组为migrated/passed，部分组保留in_progress/not_run，
  不因部分子集通过就标整个旧目录通过。
  文档链接/AGENTS长度、非补丁差异空白及Python lint/格式检查通过。
  六个原字节补丁含diff上下文前缀空白，git whitespace检查会误报；
  不修改补丁载荷，按固定SHA256验证，仅从样式空白检查中精确排除patch文件。

## 行为调整与未测边界

合法旧支持输入字节保持；CBOR范围/skip子集/失败事务语义、
SHA长度/消费后错误以及旧void非法输入处理变化见
[公共合同](../framework/common/contract.md)。不能据“对拍”宣称非法旧行为不变。
适配只覆盖迁入CBOR/SHA和错误头；不同平台ABI需重新验证，
不声称已支持整个旧固件二进制或完整TSAP验签。
机械输出检查只适用于新公共源；无物理输出不等于验证保护实现，
不替代旧L5所有规则。旧基线仍是243 passed、1 failed的记录，
不重跑移动后的旧环境、不删断言/加宽泛诊断豁免制造旧基线全绿。
未构建Zephyr固件、完整Linux宿主或Forge套件，未烧录/访问真实设备、
网络/云模型。新公共库在主机通过，不是MCU资源/停止阈值证明。
正式数据/持久格式没有改变；R02–R13都未完成。

## 重现

```sh
cmake -S . -B build/r01 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/r01
ctest --test-dir build/r01 --output-on-failure
cmake -S . -B build/r01-gcc-sanitized -G Ninja -DCMAKE_C_COMPILER=cc -DENTELEA_SANITIZERS=ON
cmake --build build/r01-gcc-sanitized
ctest --test-dir build/r01-gcc-sanitized --output-on-failure
python3 tools/check_dependencies.py --dependency zephyr --checkout ../entelea-legacy/.deps/zephyr
python3 tools/check_migration.py --sources
```

基线来源必须可访问；无法取到固定源码或依赖不报假通过。
业务/测试先提交，再将其commit填入唯一台账并提交台账更新，
不在本证据文件写自身commit造成自引用。
