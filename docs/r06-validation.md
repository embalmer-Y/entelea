# R06 网络机制/动态依赖验证

授权边界：机制+假传输（不碰真实网络/云——观测源=调用方上报，真机批
替换）；R06 depends_on R04/R05（软件面已交付，台账状态机如实保持）。

## 交付范围

- 心跳监视（断链判定源）：恢复滞回 2 连/超时 6×1000ms 立即断/部署
  模式守卫（orchestrated 才迁移安全态）。
- 控制租约：TTL 10s 惰性过期/id 不复用/同 holder 幂等续期/他人拒+
  归因回填/本地席位强制失效视图（不改在册）。
- 幂等缓存：4 槽 LRU/idem 键权威回放（不重执行）/unknown（PENDING/
  失败）不缓存 → 同 idem 重执行（重试决策归调用方）。
- 依赖状态机（新面）：network/model/storage/app 四类；恢复防抖 2 连
  good（DEGRADED 中间态可见）；失败立即 DOWN；独立查询（model down
  不绑死独立 App）。

## 验收矩阵与真实结果（2026-11-08；net 套件 4/4 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 断链判定源 | 单次心跳不恢复（滞回）；从未心跳不误报；超时立即断 | test_linkmon_hysteresis_and_guard |
| 部署守卫 | orchestrated 断链→LINKLOSS/恢复→ACTIVE（真实安全态迁移断言） | 同上 |
| 租约准入 | 空 holder 拒；他人拒+id 回填；续期 id 不变；过期重授新 id；他人 release 拒 | test_lease_admission_and_views |
| 席位视图 | LOCAL 期 acquire 拒/held_by 恒 false；释放后未过期旧租约仍有效 | 同上 |
| 幂等回放 | 同 idem 换请求体不重执行（键权威）；回放原回执字节 | test_idem_replay_and_unknown_nocache |
| unknown 不重试面 | PENDING 两次同 idem = 重执行 2 次（不缓存不吞）；replayed=false | 同上 |
| LRU 驱逐 | 5 键填 4 槽：最旧重执行、最新命中 | 同上 |
| 恢复风暴 | good/bad 交替永不 UP（防抖抑制）；DEGRADED 可见 | test_dep_state_machine_and_storm |
| 独立判定 | model down 时 net 保持 UP（不全局绑死） | 同上 |
| 假传输注入 | DNS/TLS/云不可用 = bad 观测（真实网络零接触） | 同上 |
| 回归 | 主机 97+CTest 6/6+架构 44 源 | 本轮实跑 |

## 自行决策（建议值——已追认：owner 2026-11-09 整体追认，计划 §10.3）

① 恢复防抖 2 连 good（D08 建议值——真机校准前）；② 幂等回执预算
128B（旧 512B 的机制批预算——命令全表批再调）；③ 依赖四类枚举
（app=App 声明面——更多类随消费面追加编号）；④ 无定时器（全部惰性
判定——合同 9 确定性）。

## 未测与边界（如实）

真实 DNS/TLS/云探测与 zenoh 会话重连（真机批替换观测源）；风暴阈值
真机校准（D08）；avq/pubq 传输机制（本批后续）与媒体外发决策面
（R09 闸——L207/L373；两侧未实现，归属经计划符合性审查一致化）；
命令分发全表（真机命令面）。
计划正测 L338 之『授权有界日志读取、明确幂等分块』未交付且此前
未点名——如实补记（处置归计划修订/轨道 2）。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/net \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r06-tnet/zephyr/zephyr.exe
```

## B 批（2026-11-09）：B-1 传输队列补交付

- avq=drop-oldest 媒体帧环（4×512B 静态池拷贝；溢出丢最旧+计数；
  帧超限整笔拒）；pubq=拒新发布环（8 条；满=E_BUSY；弹出仅 sink
  送达后=at-least-once 至传输面）。传输面=可注册 sink（假传输闭环；
  真传输随真网批同形状）；无定时器（flush 调用方驱动）。
- net 套件 test_avq_pubq_bounded_queues：drop-oldest 序/计数/帧超
  限/拒新/顺序/送达后弹/参数负测——5 用例全绿。
- 归属提醒：媒体外发**决策面**仍归 R09 闸（D11 未冻结）；本交付=
  传输机制（R-V 表外原"本批后续"欠条闭合）。

## F 批（2026-11-09）：真网批

### F-1 WiFi 关联+netmon 联动闸（就绪；板级 BLOCKED——到场项）

- 闸 `platform/zephyr/apps/hw_wifi_netmon/`（+forge `gate/wifi_inject.py`
  凭据串口注入驱动——P4C1 协议=旧树 E1 纪律：凭据不入镜像不入仓库，
  用后 zeroize）：connect→DHCP→**dep_report(NET,good)×2→UP（防抖）**
  →disconnect→**dep bad→DOWN（立即）** 两轮；观测源实装=R06 合同
  "真机批替换观测源"。
- 模块面：nanopb 入 pin 台账（dependencies.json+PINS；esp_hosted RPC
  编解码）；build_zephyr.py 模块白名单改**从 dependencies.json 动态读**
  （单一来源）。
- **板级 BLOCKED（如实）**：esp_hosted SDIO 枚举 -134（C5 无 CMD5
  应答）。已排除本迁移引入——**旧树自身的扫描诊断镜像（同板历史
  2/2 PASS）同构建同烧录复跑同样失败**；复位脉冲 50→500ms 无效；
  CHIP_PU（GPIO54）前置长复位实验亦无应答。C5 UART/GPIO0 未引出
  （核心板）——软件诊断面已尽。**归到场项**（与真实断电 VBUS 同批：
  断电重启 C5 是下一恢复实验；若仍无应答=固件/硬件故障，需 owner
  裁决：C5 固件重刷=旧树"经授权写入 Kaleido 留存 esp-hosted 3.0.10
  v661 成套镜像"路径——该镜像在旧树备份私有保存，重刷需另行授权）。
- 板已恢复 minimal 安全常驻态。
