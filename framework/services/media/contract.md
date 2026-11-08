# R09 可选媒体接口合同

新-only；方向与未决项以[执行规划](../../../docs/rebuild-plan.md)第 7
节为准。

## 独立可选

- 每类媒体（图像/PCM/显示）**独立绑定后端**（同一 ops 形状）；未
  绑定 = E_UNINIT（不伪装可用）。采集≠保存/外发/记忆——本面仅
  **采集**；保存/上传是独立权限（av:write/av:send）与独立面。

## 生命周期

- IDLE→CAPTURING（start；重复启动=E_BUSY 显式）；CAPTURING→IDLE
  （stop 幂等）；后端失败→FAILED（观测态——重启采集恢复）。
- 停止后 poll=E_STATE（**不复活**——取消语义）；无帧=E_BUSY
  （非失败——调用方重试面）。

## 权限

- capture_start：av:read 声明门（无授权采集拒——采集本身已受权限）。
- 帧数据只在采集上下文内交付（buffer 归调用方；借用生存期归调用方）。

## 后端形状（真后端=板级批）

- start（失败=E_IO）/poll（E_BUSY=无帧；E_IO=失败置 FAILED）/
  stop（幂等）/state（观测）。假后端（ENTELEA_TEST）验证接口与
  生命周期语义；真后端（S3 es8311/PDM、P4 显示/摄像头）随板级批。

## 未测与边界

真后端物理行为（S3 代码不当 P4 完成——板级批）；DMA 取消中清理
（真后端负测）；帧上限 512（机制批建议值——真后端按需调）；
avq 外发队列（R06 发布面）。
