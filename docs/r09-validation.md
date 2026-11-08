# R09 可选媒体接口验证

授权边界：接口与生命周期机制+假后端；真后端（S3 es8311/PDM、P4
显示/摄像头）随板级批（S3 代码不当 P4 完成——合同既有约束）。

## 交付范围

- 独立可选媒体接口（**每类独立绑定后端槽**：图像/PCM/显示类参数
  贯穿 bind/start/poll/stop/state；重绑同类=替换不影响他类——计划
  符合性审查 P2 修复，原单 static 后端二次 bind 覆盖他类；未绑定
  =UNINIT 不伪装）。
- 生命周期：重复启动 E_BUSY/停止幂等/**停止后不复活**/无帧 E_BUSY
  （非失败）/后端失败显式 FAILED 态可观测/重启恢复。
- 权限：capture_start = av:read 门（采集≠保存/外发/记忆）。
- 假后端（ENTELEA_TEST）：帧/失败注入+start 正向控制。

## 验收矩阵与真实结果（2026-11-08；media 套件 2/2 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 独立可选 | 未绑定 UNINIT；缺 ops/类外绑定拒且不改既有 | 两用例 |
| 按类独立 | 只启 IMAGE 不动 PCM（状态/计数独立）；重绑他类不影响进行中采集；帧内容不串槽 | class_independence（审查 P2 修复新增） |
| 权限门 | 未授权拒且零启动副作用（start_calls 不变） | lifecycle |
| 生命周期 | 重复启动 E_BUSY；停止幂等；停止后 poll E_STATE | lifecycle |
| 帧语义 | 帧耗尽 E_BUSY（非失败）；帧数据/序号往返 | lifecycle |
| 失败显式 | start 失败不置 CAPTURING；poll 失败置 FAILED；重启恢复 | failure |
| 回归 | 主机 97+CTest 6/6+架构 51 源 | 本轮实跑 |

## 自行决策（建议值——已追认：owner 2026-11-09 整体追认，计划 §10.3）

① 帧上限 512（机制批——真后端按需）；② 采集权限=av:read（保存/
外发独立面）；③ 假后端仅 ENTELEA_TEST（不进生产）。

## 未测与边界（如实）

真后端物理行为/DMA 取消清理（板级批）；帧上限真机校准。
**未交付面（计划 L374-377，如实点名——处置归计划修订/轨道 2）**：
播放/显示接口面、多消费者寿命、预算面。
avq 外发归属（审查 P2 一致化）：媒体外发决策/内容面归本批闸
（L207/L373）；avq/pubq 传输机制归 R06 传输面——两侧均未实现。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/media \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r09-tmedia/zephyr/zephyr.exe
```
