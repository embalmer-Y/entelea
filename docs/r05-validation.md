# R05 受限 App 与包事实验证

授权边界：R05 按 owner 目标指令自主推进（depends_on R03/R04——R03 软件
面已交付；R04 软件面已交付、台账批次状态机因 R02 硬件闸如实保持 pending，
机制依赖已就绪）。D03 已确认为"首期设计覆盖+关键可行性验证"。

## 交付范围（子闭环 1：包事实；子闭环 2：Lua 合同；子闭环 3：Wasm 闭环）

- TSAP v2 容器（字节格式与旧版逐字节一致）：头 oracle（magic/v1/未知
  flags/零长/截断 fail-closed）。
- 分步安装链 + 全量验签管线（tweetnacl 逐字节未改+PROVENANCE）+ 多根
  信任集 + 签A装B防御 + activate 经 meta 双副本。
- Wasm 最小闭环：WAMR pinned 25bd7eb（fast 解释器+指令配额+WASI 关+
  池）——load（验签复核）→ call（步预算有界停止）→ stop（实例销毁/
  模块保留换包路径）；代表 native 宿主 principal 注入裁决。
- D03 Lua 接入合同：载荷识别/内存预算闸/执行预算有界停止/数据隔离/
  工厂产物编码（机制验证——无解释器依赖，不冒充正式支持）。

## 验收矩阵与真实结果（2026-11-08；pkg 7/7 + wasm 1/1 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| TSAP v2 fail-closed | magic/v1/未知flags/零长段/截断全拒 | pkg test_tsap_header_oracle |
| 验签全管线 | 好签名过（信任集+显式键）；位腐拒；签A装B拒；坏根拒；信任表路径过 | pkg test_verify_good_and_bad |
| 安装链 | 越界 chunk 拒；activate/active 往返 | pkg test_stage_chain_and_activate |
| Lua 载荷识别 | Wasm magic 拒（不默认解释）；截断拒 | pkg test_lua_payload_ident |
| Lua 内存预算 | 超配额拒+计数；恰满过；释放后峰值保留；实例独立配额 | pkg test_lua_mem_budget_gate |
| Lua 有界停止 | 预算 10 步恰耗尽 false；aborted 后续步全拒 | pkg test_lua_exec_budget_bounded_stop |
| Lua 工厂编码 | 域外版本/内存/步数全 PARAM | pkg test_lua_contract_encode |
| Wasm 签名装载 | 坏签名 slot E_INVALID_SIG（装载前复核） | wasm test_signed_load_call_stop |
| Wasm 执行闭环 | 好包 load→call→ret≥0；换包重装载 | 同上 |
| Wasm 有界停止 | 步预算 0 → E_TIMEOUT+实例销毁；停止后调用 NOTFOUND | 同上 |
| 回归 | 主机 97 项+CTest 6/6+架构 42 源 | 本轮实跑 |

## 自行决策（建议值——已追认：owner 2026-11-09 整体追认，计划 §10.3）

① 测试 wasm 模块用旧 wamr/app 固定夹具 sample.wasm（103B）——真死循环
模块随工厂夹具对拍（零预算已证机制面）；② 验签缓冲 4KB 上限（档位随
板调——旧 K5 语义）；③ natives 最小集（1 代表 native）——全符号集随
消费面扩展；④ WAMR 源编入 app 目标（app 模式独立库不保序）。

## 未测与边界（如实）

真 Forge/设备字节对拍（R12 工厂闭环）；多实例并发（V1 单实例）；
natives 全符号集（R06+）；线程/共享内存（DEC-55）；Lua 解释器接入
（未批准依赖——机制合同已备）；真机执行（native_sim 证明软件路径）。

## 复现

```sh
for app in pkg wasm; do
python3 tools/build_zephyr.py --app platform/zephyr/tests/$app \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r05-t$app/zephyr/zephyr.exe
done
```

## 计划符合性审查补记（2026-11-08，轨道 1）

- 新增判别性用例 test_trust_table_only_key：生成**非工厂**密钥对——
  入表前验签拒、仅写入信任表后过、并集语义（工厂根不受表内容影响）
  三面（原用例把测试根写入表而工厂根==测试根，表键分支无判别力）。
- appstore.h activate 合同字面已对齐实现（激活=包事实记录，验签
  前置不存在——装载链复核强制安全；原"仅验签通过后可调"失实）。
- wasm 代表 native 裁决：矛盾注释/断言订正；新增 test_native_probe_
  principal_gate 经直达钩子覆盖授权=1/未授权=E_PERM/伪造句柄=E_PERM
  （sample.wasm 无导入——VM 内 wasm→native 调用路径随工厂夹具，不冒充）。
- 如实点名（处置归计划修订/轨道 2）：设备/Forge 字节对拍记"归 R12"
  但计划文本未修订（L311 输出项）；D03 首期七项之"宿主调用与可信
  身份"在 Lua 合同无条款亦未记为不适项。

## Owner 决议执行（2026-11-09）

- **A3**：D03"宿主调用与可信身份"条款补入本目录 contract.md（Lua
  侧与 Wasm natives 同一注入 principal 纪律；机制面经
  entelea_wasm_test_probe_decide 佐证，Lua 侧随正式支持批实测）。
- tweetnacl 上游独立对拍完成（D 授权后公开网络拉取）：.c/.h 两文件
  sha256 与上游逐字节一致——PROVENANCE 已登记独立证据（原为继承
  旧树主张）。
