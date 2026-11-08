# Entelea 新 Agent 接手 prompt

以下内容可直接发给新 Agent。本文件是 2026-11-08 的交接快照和工作入口，
不是第二份架构或进度台账。后续状态以实际 Git、重建规划和唯一资产台账为准；
遇到差异先核对来源与用户新授权，不重置到本快照。

---

你现在接手 Entelea 的迁移重建。我会把 `/home/emb/project` 作为工作区。
请先理解和核对当前状态，再与我确认下一批；**首轮只做接手核对和必要的
只读/主机验证，不修改实现、不自行启动 R02 或任何后续批次。**
此前授权是“完成 R01，验收后停止，再讨论下一批”，交接不扩大此授权。

## 1. 工作目录、仓库与固定来源

| 位置 | 用途与操作边界 |
| --- | --- |
| `/home/emb/project` | 工作区根，不是 Git 仓库；根 AGENTS 仅作路由，不在此创建仓库 |
| `/home/emb/project/entelea` | 唯一新版框架/产品实现，当前本地分支 `rebuild`；工厂仍保持独立工程边界 |
| `/home/emb/project/entelea-legacy` | 冻结旧主仓库，`main` 留在基线；只读查固定对象，不修源码或清缓存 |
| `/home/emb/project/entelea-legacy/agent` | 固定 Forge 子仓库；独立工具工程，不是新版设备大脑 |
| `/home/emb/project-archive/entelea-handoff-2026-11-08` | 11个原顶层旁支目录的外移位置，仅按需参考，不全量扫描/重新导入 |
| `/home/emb/zephyr-sdk-1.0.1` | 当前机器已有 SDK 的只读参考位置，未移动；不是跨机器硬编码要求 |

**新版是 linked worktree，共享 Git 对象库在 `entelea-legacy/.git`。
禁止删除或再次移动旧目录；也不能只复制新版文件就声称已携带完整 Git 历史。**
Forge 的 Git 存储也在旧主仓库内，必须保持仓库边界。
不要照搬归档 AGENTS/Makefile 的旧实时开发流程覆盖新版规则。

固定身份：

- 旧主基线：`e54047b51519fa449dfe4a338af8d126bed8fac4`；
  本地 annotated 标签：`legacy-baseline-2026-11-08`。
- Forge 基线：`ca5219e7cf6929749b699d8d82da33cdc4eb463e`。
- R00 新骨架提交：`c4a182a5433d56b1530a6a0021d7120409d71e43`。
- R01 实现提交：`f9fcba53d89a0e7f76ceeb6bdf2c8d6616e3ded7`。
- R01 台账提交：`4dc1a215eb316bb7368bb17102fc2298ed0eff1f`。
  交接文档在其后追加；这个值不是要求永远保持的新版 HEAD。

首轮请实时核对分支/HEAD、标签解析、worktree 关系和三个仓库工作树。
使用 `git -C /home/emb/project/entelea ...`，或先进入新版目录再执行命令。
发现未提交修改则保留并判断是否冲突，不覆盖、reset、amend 或替别人提交。
上述本地基线未推送；不能假定远端旧 main 能取到它们。

## 2. 权威来源与阅读顺序

先读新版 [AGENTS](../AGENTS.md)、[README](../README.md)，然后：

1. [重建规划](rebuild-plan.md)第1节、第5–7节：当前状态、批次依赖、
   执行规程和方向评审；待我确认批次后精读该批完整条目。
2. [R01 验证记录](r01-validation.md)和
   [公共机制合同](../framework/common/contract.md)：已实现行为及未测边界。
3. [构建说明](../build-support/README.md)、
   [固定依赖](../build-support/dependencies.json)、
   [源分类](../build-support/source-policy.json)：实际入口和机械约束范围。
4. [唯一资产台账](migration-assets.json)：定向查询 `batches`、相关 `assets`、
   `modules`、`source_files`、`design_decisions`；**不要整份读入上下文**。

旧基线的权威定义按需从 Git 对象读取：

- `docs/internal/p05-framework.md`：已确认框架定义。
- `docs/internal/p05-first-product.md`：已确认首个验收产品定义。
- `docs/architecture.md`、`requirements.md`、`testing.md`：
  旧实际实现/旧 P/H/I 进度；不要当成新版完成状态。
- `docs/internal/p05-design-{proposal,runtime,cognition,delivery}.md`：
  深度调研后的设计建议，不是全部冻结合同。
- `docs/internal/rebuild-baseline-validation.md`、`docs/provenance.md`：
  旧失败、依赖出处和真实验收范围。

例如在新版目录运行：

```sh
git show legacy-baseline-2026-11-08:docs/internal/p05-framework.md
git show legacy-baseline-2026-11-08:firmware/module/tessera/src/core/events.c
git -C ../entelea-legacy/agent show ca5219e7cf6929749b699d8d82da33cdc4eb463e:tessera_agent/creation/loop.py
```

定义、建议、实际代码、验证证据是不同层次；用户确认的共识优先。
当前获批窄合同指导新实现，修改共识或首期范围先与我讨论并记录。
默认上下文只包含根入口、当前批次、相关资产及必要合同；不要复制旧讨论，
新建同义计划/状态表或一次性读完800项清单、旧工程、日志及依赖树。

## 3. 项目目标与已确认的不变量

历史上 Galatea 是原始目标，Tessera 服务于它，Kaleido 后来合并作参考。
旧合并树复杂，所以采用“冻结旧基线、干净新版、逐机制按需迁入”，
不是整目录重命名或全量复制。新框架统一叫 Entelea，主要由 AI 开发；
清晰的模块合同、机械验收和短上下文比堆文档/抽象层更重要。

- 面向 Zephyr MCU 与原生 Linux ARM64/x86。MCU 是资源有限的智能硬件；
  Linux 可承载更完整大脑，共享语义和适用机制，不统一包装整个 OS。
- 采用模块化单体与按需窄 ops，不做微服务、service locator、万能管理器
  或万能语言插件系统；目录是建议目标，不为终态预建一堆空目录。
- 框架层称“辅助智能/模拟智能”，“伴侣”只用于产品场景。
  设备角色、认知能力、行动权限独立，不能由硬件性能推导权限。
- 模型只是建议者，不能伪造可信 principal、来源或能力；
  执行必须经过授权、资源准入和最终输出保护。
- 明确身份、来源、时基、状态单写者、借用寿命、失败域和有界预算。
  accepted 不等于 completed，取消不撤回已发生效果，
  unknown 非幂等动作不得自动重试。
- 云模型不可用就停止智能；固定功能/独立 App 按真实依赖继续。
  不制造假离线智能，不把云失败和物理安全故障混为一套状态机。
- 经历、情感、图像、音频、显示各自可选；采集不等于保存/外发/记忆。
  框架媒体只做必要窄接口，不提前承诺完整媒体图、打断或 AEC。
- 启用经历的配置/伴侣产品要求 >8 GB TF；不是所有框架配置的最低硬件。
  缺卡或不可恢复时只留安全/设置/修复，不自动创建空角色掩盖故障。
- 新能力经过具体候选工程验证、受控试用、用户对内容/权限确认才正式启用。
  开发配置可提高有界预算/直接试运行候选，不绕验签、权限、保护或外发政策。
- 首个完整产品是 MCU 桌面智能伴侣。首期 Linux 只交付最小原生宿主/
  同源合同及 Wasm 证明，不是完整 Linux 伴侣；跨设备控制延期。

## 4. 当前已完成与尚未完成

**R00、R01 done；R02–R13 pending。没有在执行或未收尾的业务任务。**
R01 是公共机制和主机构建，不是设备运行时、驱动或可运行产品：

- `framework/include/entelea`：错误、来源/可信身份、时间、App 表示、CBOR/SHA。
- `framework/common`：实例时间/截止时间、独立 CBOR/SHA 机制及一份合同。
- `framework/compat`：所迁旧子集的真实 `ts_cbor_*`/`ts_sha256*` 链接符号及错误头，
  不是整个历史固件 ABI 兼容层；默认纯公共库不链接它。
- 根 CMake/CTest、三依赖 pin、六个原字节补丁、只读依赖/来源核验、
  公共依赖限制和产品/诊断精确分类、行为/治理负测。

台账快照：800个固定来源条目（旧主555、Forge245），115个资产组；
13个旧源项实际提取/适配（含六补丁），另有新-only `N-CONTRACT`。
`A-CBOR`、`A-SHA`、`N-CONTRACT` 已 migrated/passed；
部分头/脚本/规则组仍 in_progress/not_run，不因子集通过变成整组完成。
`A-NET`/`A-STORE` 提前提取的是共享声明，网络/存储业务没有迁入。

历史已通过：GCC C11 Debug 6/6 CTest、GCC ASan/UBSan 6/6、
48项 Python 治理、纯公共库 Release、公共/旧子集头 C++11、固定旧源对拍、
SHA 独立 oracle、三个旧依赖只读核验、800项来源核验。
复验仍须用实际输出，不把此历史记录当作未来代码永久通过。

未实现/未验收：完整 TSAP/COSE schema/验签、任务/权限/资源闭环、
Wasm/Lua 运行时及预算/停止、Zephyr 平台/物理保护、完整 Linux 宿主、
网络/云模型、经历/情感/媒体后端、试用晋升、正式产品。

## 5. 接续设计时易误读的 R01 合同

请以公共合同和头文件为准，尤其注意：

- `entelea_result_t` 的 `-13` 是显式 pending，不把所有负数一律当失败。
  旧 `TS_E_*` 是字面数字宏，保留预处理 `#if` 语义。
- principal 不透明，`source.id` 不是权限；结构体/类型存在不等于授权已实现。
- 单调时间按实例注入；0 ms 合法，invalid 与0不同。不同 clock_id 不比较；
  回退/无绑定/溢出显式失败，失败不改输出；跨重启必须处理旧截止时间。
- Wasm/Lua 显式区分载荷种类/版本与共享生命周期；不同 VM 的 steps 不等价，
  deadline 不替代执行预算。R01 只有表示，没有 VM 中断或任务状态机。
- CBOR 是有限 definite 子集，不自动排序 map、不校验 UTF-8/重复键；
  容器 open 只读头，最终 rem/schema 由消费方检查。借用 buffer 必须活着；
  失败不推进游标或留下部分写，完整严格签名 schema 留 R05。
- SHA 是摘要，不是验签；流式长度有界，final 消耗上下文，非法调用显式失败。
  旧 void 适配的 stderr/abort 不是默认 MCU 错误路径；R02 要明确平台策略。
- 合法旧支持输入保持字节；CBOR 有符号越界/skip 拒绝子集、失败事务及
  SHA 长度/final 错误有已记录调整。不要宣称所有非法旧行为也不变。
- 不将 C enum/pointer/sizeof 结构体直接序列化成协议，不擅改 TSAP/持久格式。
- `source-policy.json` 是 CMake 消费的实际精确源表；每个适用 C 源恰分类一次。
  诊断为独立精确目标/源，EXCLUDE_FROM_ALL，不能混生产或做宽泛白名单。
  当前保护输出源列表为空；无输出只能证明公共机制未旁路，不是物理保护验收。

## 6. 下一批与未决设计

D01–D12 都是 `direction_approved`，**不等于所有细节冻结**。
不要重复询问已批准方向；读取规划第7节，确认当前批次未决接口/行为/数值。
尤其 D03：首期必须覆盖 Lua 接入合同及关键可行性验证，R05 负责代表性
预算/停止/隔离验证；正式支持可后续交付，不可删掉首期验证或伪称已经支持。

R02/R03 在 R01 后各自 ready，但“依赖 ready”不是我已授权你实现：

- R02：Zephyr 最小启动/监督/不可绕输出保护；先冻结参考板、负载、
  测量方法、停止/WDT阈值及安全状态，再落实构建和平台机制。
  保留旧同步事件、ISR 指针借用、驱动引用寿命、锁序及检查/提交原子性。
  native_sim 只证明软件路径，不能替代上电/复位/失联/物理故障停止。
- R03：存储/配置/审计；先确认 namespace、恢复/掉电、错误可见性、
  数据类别与凭据撤销/重置；不是经历库，不能自动转空角色或不可逆格式。

之后：R04 能力/任务/资源；R05 App/包；R06 网络/动态依赖；
R07 Linux；R08 辅助闭环；R09 媒体；R10 经历；R11 情感/适应；
R12 工厂/试用/晋升；R13 配置与产品验收。
**真实依赖只看台账 `batches.depends_on`，不是按这个列表顺序猜测。**
不得静默跳前序、扩大当前批次，或用 R 阶段替代旧 P/H/I 进度。

## 7. 新 Zephyr 构建与 SDK 边界

用户在本次交接专门确认了构建说明中的目录合同：

- 新版受管理源码按需放 `entelea/.deps/<dependency>`；不得整搬旧
  `zephyrproject` 或所有依赖。完整核验的外部 checkout 可显式只读复用。
- 产物放 `entelea/build/<batch>-<target>`；R01 保留现有路径。
  native/板级/诊断/实验目标各自独立，不用旧 CMake/Twister 缓存或全局 west。
- SDK 在 Project 外；本机参考 SDK `1.0.1`、RISC-V GCC `14.3.0`
  已只读核实。显式 `ZEPHYR_SDK_INSTALL_DIR`，不靠目录名或自动发现认定版本。
  目标 SDK/工具链组合仍随 R02 实际参考板冻结，不批准所有未来板卡。
- 构建前必须验证实际源码/完整补丁、SDK 实际版本、所需编译器/模块；
  native 用适用 host，板级显式 zephyr 工具链；缺失/不匹配/实验污染显式失败，
  不自动安装、下载或回退。不要修改共享 SDK、shell 配置或旧归档。
- **这些是已确认规划，不是已实现构建脚本/SDK 自动校验。**
  R02 第一项相关工作要落实入口及负测，当前新版不能构建 Zephyr 固件。

已有来源核验 pin 在 dependencies.json：Zephyr `41d10b59730406bbab4ca9f1fbbe9e96aafb4966`，
WAMR `25bd7eb63e828e4bd242cc9b38d260b4b31c6605`，
HAL `cc81db00233af6020f2fea9fbb0d0098fb3d0d86`。
六个批准补丁保持原字节/顺序/摘要；GDMA/CPLL 实验不在批准集合。
现有依赖核验器是只读的，不准备源码，也不校验 SDK 或实际固件构建。

## 8. 外移资产与既有失败

外移目录保留原名：Galatea、Tessera、Kaleido、entelea-forge、deps、
zephyrproject、agent-venv、tools、build、logs、.pytest_cache。
固定800项来源仍只指向 legacy 和其中的固定 Forge；不要误用外移的
`entelea-forge` 代替 pinned Forge，也不要把旧项目未提交内容当成新基线。
Galatea/Kaleido 的本机位置已登记在台账 `external_references`。
搬迁记录在外移目录的 `relocation-manifest.json`，不是开发进度表。

搬迁只改目录位置，原样保留历史/隐藏/未跟踪内容：Kaleido 六个未跟踪文档、
旧 Forge 未跟踪文件未提交。22个嵌入式组件的 Git 指针在搬迁前就已无效，
已记录并保持原字节，不是这次引入的故障，不要顺便修复。
旧虚拟环境/shebang/缓存可能仍有旧绝对路径；保存资产不等于旧环境仍能运行。
旧 zephyrproject 有 DMA 实验修改，即使提交相同也不符合新版批准集合。
不要读取、输出或上传 logs/keys、凭据及私有配置内容。

旧基线并非全绿：`make check` 到 L5 失败；独立 pytest 为
243 passed、1 failed（`test_safety_contract_self.py::test_repo_smoke_green`）。
p4acoustic 三处和 p4audiopc 一处 GPIO 输出触发同一机械检查根因。
没有改旧源码或扩大白名单；不能用新树空输出的绿灯制造“旧L5/物理安全已通过”。
完整 Forge/native_sim/P4/真实网络/模型/硬件未继承为本次验收。

## 9. 首轮主机复验入口

从新版目录执行，只编译公共机制、检查固定来源；不联网、不操作硬件：

```sh
cmake -S . -B build/r01 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/r01
ctest --test-dir build/r01 --output-on-failure
python3 tools/check_dependencies.py --dependency zephyr --checkout ../entelea-legacy/.deps/zephyr
python3 tools/check_dependencies.py --dependency wamr --checkout ../entelea-legacy/.deps/wamr
python3 tools/check_dependencies.py --dependency hal_espressif --checkout ../entelea-legacy/.deps/hal_espressif
```

CTest 已包含公共 C 合同、架构、800项来源、48项治理、依赖元数据和旧源对拍；
不必同时重复跑全部 Python 测试。仅改治理时可用：

```sh
python3 tools/check_migration.py --sources
python3 -m unittest discover -s tools/tests -v
```

需要行为验证时再选适用 sanitizer/纯库/平台目标，命令见 R01 验证记录。
本机 Clang18 普通编译可用但缺 ASan runtime；已有 GCC sanitizer 验证通过，
不安装共享工具或禁用检查掩盖失败。没有固定来源/依赖则明确报告缺失，
不凭这份 prompt 的历史数字报成功，也不自动重建旧环境/调用云。

## 10. 获批后的每批交付规程

1. 确认一个 ready 批次、允许文件、具体迁移源项、兼容边界、决策闸与验收。
2. 深读将迁入的旧行为/耦合；800项对象盘点不是800项逐行审计。
   先提确定性机制，再平台适配，同批迁入所需正/负测及邻近单份合同。
3. 保持稳定源 ID、repo/commit/tree/object/SHA；路径变化不改变来源身份。
   `first_batch` 只表示首次审查/复用，不表示该旧目录要一次迁完。
4. 源项选择 `pending_review/migrate/reference/defer/discard`；
   migrate 记录精确目标/可解析本地实现 commit；discard 不删除旧资产。
   一文件多批拆分累计目标，部分组保持 in_progress。
5. 迁移状态与验证状态分别记录。新-only 用明确 `N-*` 及实现 commit/
   目标/证据，不伪称来自旧实现；格式规则看现有 checker/台账先例。
6. 先提交实现/测试/证据，再填其 commit 并提交台账，避免 commit 自引用。
   保留许可/SPDX；本地提交带 `Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>`。
   涉及 Forge 变更时先确认新的独立工程工作区，冻结 `entelea-legacy/agent` 保持只读；
   在独立工程单独提交并记录版本/兼容关系，不扁平化进固件或误改固定副本。
7. 所有必需合同/闸通过才 done；未测、blocked、失败及限制如实记录。
   不吞错、改 oracle、删断言、扩大排除或用成功形默认值制造通过。
8. 不推送、改远端默认分支、烧录、访问真实硬件、真实云/付费模型或外发
   用户数据，除非我另外明确授权；工程通过不等于产品用户验收。

## 11. 你接手后的第一份回复

完成阅读/核对后，先给我一份简洁报告：

- 实际工作目录、分支、HEAD/固定标签/Forge 与 worktree 关系是否正确；
  有无现有修改、冲突、缺失来源或环境问题。
- R00/R01 已完成、后续未启动；主机复验的真实结果与未测范围。
- 已确认架构方向，尤其框架/产品分层、Lua 首期验证和输出保护边界。
- 下一批建议（R02 或 R03）及其必须由我确认的具体事项，说明不自行选择。

然后等待我确认下一批范围/未决边界；不要把“请接手”理解为自动重建整个项目。
