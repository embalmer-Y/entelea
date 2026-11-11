# R05 包事实/验签/Wasm 宿主/Lua 接入合同

迁自旧 appmgr 的 R05 子集 + 新-only 面；方向与未决项以
[执行规划](../../../docs/rebuild-plan.md)第 7 节为准。

## TSAP 容器（字节格式与旧版逐字节一致——对拍 oracle 前提）

- v2（fmt_ver=2 + flags=1）：头 16B（大端）+ 32B sha256(manifest‖wasm)
  + manifest(CBOR) ‖ wasm ‖ COSE_Sign1(ed25519)。v1/未知 flags/零长段/
  溢出布局 fail-closed 拒收（无兼容装载）。

## 分步安装链（staging→verify→activate）

- stage_begin（擦 slot）→ stage_chunk（顺序写+回读；越界拒）→
  verify → activate（meta 双副本记录 "ACTV<slot>"）。
- 验签管线：摘要重哈希 → COSE_Sign1 精确子集（tag18+array4+phdr
  {1:-8}+map(0)+payload+sig(64)）→ 内嵌 payload 副本逐块比对裸段
  （签 A 装 B 防御）→ tweetnacl ed25519（逐字节未改上游；PROVENANCE
  登记）。多根信任集：工厂根（TEST=固定测试根 / 生产=prov pk0+pk1）
  ∪ R03 信任表（去重），任一过即过。读错误 = E_IO，格式/签名不符 =
  E_INVALID_SIG，两者不互相改写（RF1）。
- 装载前复核验签（wasm_load 双层纪律——本层不信任"曾经验证过"）。

## Wasm 宿主（最小闭环）

- WAMR pinned 25bd7eb（DEC-17/25/31/48③ 编译面：fast 解释器+指令配额
  +WASI 关+池模式；256KB 池）。app 模式下 WAMR 源直接编入 app 目标
  （独立 zephyr 库不保序且不入链——上游 §878 语义）。
- 生命周期：load(slot, owner)（V1 单实例：已有活实例 = E_BUSY 且零
  副作用；验签复核，错误原样传播；64KB 装载上限 = E_RANGE；owner 此时
  绑定）→ call → stop（实例销毁，模块保留至下次 load 卸旧重建）。
- call：ctx ≠ owner = E_PERM；导出 ABI 仅 ()->i32 / (i32)->i32（前者
  arg 必须为 0，后者收到 arg），其余 = E_PARAM——这些拒绝不执行、不销毁
  实例。**步预算指令配额**（1000 指令/ms，封顶 1e8）：到期 → E_TIMEOUT
  有界停止；其余 trap → E_IO；两者都销毁实例。
- natives：只按 load 绑定的 owner 裁决（user_data；Wasm 传入整数不可转
  principal；owner={0} 未绑定 = 一律拒）。natives 注册失败 = init E_IO 并
  回滚运行时。代表 native "entelea"."host_probe" (i)i（gpio:read:0 门），
  VM→native 真路径由导入夹具覆盖；全符号集随 R06+ 消费面扩展。

## D03 Lua 接入合同（关键可行性验证——不冒充正式支持）

- **载荷识别**："\x1bLua"+版本字节（5.3=0x53/5.4=0x54）字节级识别；
  非 Lua 载荷 fail-closed（不默认解释为 Wasm、不按后缀推断权限）。
- **内存预算**：每实例配额 + 分配闸（lua_Allocator 挂点语义——
  alloc/realloc/free 钩子；超配额拒+计数；实例隔离=独立配额）。
- **执行预算与有界停止**：指令计数闸（sethook count 语义——超限
  false+aborted 置位；此后一切步拒——有界；deadline 不替代执行预算）。
- **错误/回执**：超限=E_TIMEOUT（与 Wasm 同错误面）；超配额=VM OOM
  路径（计数可观）。
- **数据隔离**：实例间内存不共享（配额独立+无共享池旁路）。
- **宿主调用与可信身份**（D03 首期七项之一，owner 2026-11-09 A3
  决议补条款）：Lua 侧宿主调用与 Wasm natives 同一纪律——**仅信
  宿主注入的 principal 裁决**（R04 `entelea_perm_check` 面）；
  脚本传入值不可构造/转呈身份，未授权=E_PERM（不崩溃）；授权
  判定与内存/执行预算闸同序先行。机制面由 Wasm 代表 native
  （`entelea_wasm_test_probe_decide`）佐证；Lua 侧同一形状随正式
  运行支持批（D03 后续）落地实测。
- **工厂产物描述**：确定性编码，与 Forge build_manifest 逐字节一致：
  map(4){0:version,1:mem_kb,2:steps,3:fmt(lua=1)}，int 键升序、整数
  最短编码；域外或未知版本/格式 = PARAM（RF1 前该编码为畸形 CBOR）。
- **如实边界**：无解释器依赖（未批准新依赖）；真 Lua VM 停止延迟
  实测归正式支持批次；本验证证明接入合同的机制可行性。

## 未测与边界

Forge/设备字节对拍仅以同一组边界向量各自验证（Forge 无构建期自动
对拍）；多实例并发（V1 单实例）；
natives 全符号集（R06+ 消费面驱动）；线程/共享内存（DEC-55 面归
后续）；Lua 正式运行支持交付批次（D03 建议=R08 前）。
