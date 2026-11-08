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
  ∪ R03 信任表（去重），任一过即过。
- 装载前复核验签（wasm_load 双层纪律——本层不信任"曾经验证过"）。

## Wasm 宿主（最小闭环）

- WAMR pinned 25bd7eb（DEC-17/25/31/48③ 编译面：fast 解释器+指令配额
  +WASI 关+池模式；256KB 池）。app 模式下 WAMR 源直接编入 app 目标
  （独立 zephyr 库不保序且不入链——上游 §878 语义）。
- 生命周期：load（验签复核+64KB 装载上限）→ instantiate（模块对象
  rt 级持有——换包重启路径语义）→ call（导出函数+**步预算指令配额**：
  超限 → E_TIMEOUT 有界停止+实例销毁，不静默续跑）→ stop。
- natives：仅信宿主注入 principal（user_data 注入面；Wasm 传入整数
  不可转 principal——与 R04 同纪律）。本闭环 1 个代表 native
  （host_probe：gpio:read:0 门证明）；全符号集随 R06+ 消费面扩展。

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
- **工厂产物描述**：合同结构体确定性编码（map{version,mem_kb,steps,
  fmt}；域外=PARAM）——R12 Forge 对拍面。
- **如实边界**：无解释器依赖（未批准新依赖）；真 Lua VM 停止延迟
  实测归正式支持批次；本验证证明接入合同的机制可行性。

## 未测与边界

真实 Forge/设备字节对拍（工厂闭环归 R12）；多实例并发（V1 单实例）；
natives 全符号集（R06+ 消费面驱动）；线程/共享内存（DEC-55 面归
后续）；Lua 正式运行支持交付批次（D03 建议=R08 前）。
