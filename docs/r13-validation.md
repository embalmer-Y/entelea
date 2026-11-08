# R13 配置装配与框架合同验收

授权边界：**框架合同验收**（native_sim 面）；产品验收（MCU 桌面伴侣
在真实硬件）=owner 硬件闸（与 R02 硬件闸同批）。Linux 侧=D02 批准的
框架证明（R07 HOST SUITE）——非完整 Linux 产品（延期面保持）。

## 交付范围

- 配置矩阵（`entelea/config_matrix.h`）：机制面独立可选组合与
  **自洽约束**（delivery/memory 需 storage；affect 需 memory）；
  命名配置 minimal-assist / full-simulated（机制验证面）。
- 框架合同验收套件（r13）：每已交付机制面一项代表性合同断言。

## 验收矩阵与真实结果（2026-11-08；r13 套件 2/2 实跑）

| 承诺 | 负测（失败面） | 证据 |
| --- | --- | --- |
| 配置自洽 | 试用无存储/情感无经历不自洽；NULL 拒 | test_config_matrix |
| R02 保护 | 安全态 commit 拒 | test_cross_mechanism |
| R04 权限/任务 | 伪造句柄拒；RUNNING 取消=UNKNOWN | 同上 |
| R06 网络 | 恢复防抖（单次=DEGRADED） | 同上 |
| R08 辅助 | 云停用（net 断→停） | 同上 |
| R10 经历 | 无同意不保存 | 同上 |
| R11 情感 | 确定性中位基线 | 同上 |
| R12 试用 | 确认后 ACTIVE；单候选 BUSY | 同上 |
| 回归 | 主机 97+CTest 6/6+架构 59 源 | 本轮实跑 |

## 未测与边界（如实——产品验收清单）

真实硬件产品验收（owner 闸）：上电/复位/失联/物理故障停止实测、
TF 故障不新建空角色、外设失效不越权/不误报完成、试用/删除/断电/
解绑/撤销与恢复、完整辅助与模拟配置闭环、签名新能力交付、经历
连续性——全部未测（真机批）。跨设备控制延期（规划既定）。
未决单帧视觉/打断/AEC（D11）不因接口存在自动算通过。

## 复现

```sh
python3 tools/build_zephyr.py --app platform/zephyr/tests/r13 \
  --board native_sim --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 --build
./build/r13-taccept/zephyr/zephyr.exe
```
