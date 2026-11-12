# Forge 工作区（工厂线）

Owner 授权（rebuild-plan.md §10.4-D1/D2，2026-11-09）：OpenHands
Software Agent SDK 为编程引擎；本目录 = Forge 工厂面，2026-11-12 并入 entelea 仓库的
`factory/forge/`（快照自 ec24f00；历史 bundle：`/home/emb/project/forge-ec24f00.bundle`）。

## 边界与纪律

- **不修改** `entelea-legacy`（冻结）；entelea 中的设备侧代码只读消费本目录产物。
- **密钥纪律**：仅使用公开测试材料（旧树 fixtures 测试根——测试信任
  ≠生产信任，Q-29④）。生产信任根/设备 prov 私钥不存在于本工作区。
- **凭据**：云凭据只经 `/home/emb/project/.cloud-credentials.json`
  读取，不复制进本目录、不入任何仓库（§10.4-D2）。
- **费用**：工厂线软分配 ≤2/3 总额度（≈18M token）；逐笔记
  `/home/emb/project/.cloud-usage/ledger.md`；80% 预警、上限硬停。
  机器执行（RF1）：`packer/run_factory.py` 是唯一 SDK 入口——导入 SDK
  前汇总台账“工厂”行（不可解析=拒），剩余额度不足单次上限即拒；单次
  token 硬上限（默认 2M，经 SDK `max_budget_per_run`，可超至多一步）；
  `run_dir` 须为未产出的 `artifacts/<run>/`（历史 run 不可覆盖）；台账
  行在 finally 写入（用量不可读按单次上限记）。`run_factory_fix*.py`
  已退役（首语句拒绝）。离线负测：`python3 -m unittest
  test_run_factory_budget`（在 `packer/` 下执行）。
- **外发**：法律范围内目的地不设白名单；仅安全下限=信任根/签名
  私钥/凭据不出本机。

## 布局

- `packer/` — TSAP v2 打包签名器（字节格式与设备 R05 验签管线对拍）
- `tasks/` — SDK 任务书（工厂 App 规格说明）
- `artifacts/` — 工厂产物（按 run 编号：.tsap 包 + candidate.json + C 头）。
  run001–003 为历史证据（run003 的 spin 对 N>=1 死循环，勿再消费）；
  run004 = `wasm_gen.py` 确定性组装 + 公开测试根签名（RF1，非 SDK 直产、
  无云调用），是设备 `hw_factory_intake` 默认头。
- `gate/gate_run.sh` 默认仅运行两个 native_sim 模式及 PENDING 跨重启
  检查，并在独立临时目录保存日志和模拟闪存；板级模式会烧录设备，
  必须先获得所有者真机授权，
  再明确指定 `board` 或 `all` 并设置 `ENTELEA_HARDWARE_AUTH=1`。
  product 模式仅证明超时后持续拒绝，不能代替用户确认或物理验收。

## 工具链

- SDK venv：`/home/emb/project/envs/openhands`（Python 3.12，由本目录的
  `requirements-openhands.lock`（带哈希）同步；openhands-sdk==1.60.2，pin 见 build-support/factory-tools.json）
- 打包器依赖：pynacl（ed25519——与设备 tweetnacl 实现路径独立互验）
