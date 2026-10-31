# tools/board · 板级联调工具（PC 侧）

> **性质**：真机联调/验收驱动脚本（PC 侧跑，经 USB 串口 + zenoh 与板交互）。
> **历史**：WSL 时代曾登记「34 脚本入仓」两度失实（b1a31e4——根因 = `.gitignore`
> 整目录忽略 `/tools/`，`git add -A` 静默跳过；2026-10-17 随 fw-v0.9.0 环境审查批
> 修复：gitignore 摘除 + 本清单入仓）。现清单 = 当前 era 实际使用面。
> **凭据纪律**：所有脚本零内嵌凭据——WiFi/LLM 凭据经环境变量（见下「环境契约」）；
> 脚本内 192.168.2.91 为实验室私网路由地址（非凭据，换网段按需改）。

## 环境契约

```bash
# ~/project/logs/env.sh（600 仓外——新机器自建）
export TS_WIFI_SSID=...        # 板级构建注入（CMakeLists 空守卫 fail-closed）
export TS_WIFI_PASSWORD=...
export ANTHROPIC_BASE_URL=...  # Agent app_develop 真 LLM 端点（Anthropic 兼容）
export ANTHROPIC_API_KEY=...
```

**zenohd 路由器**（PC 侧，二选一按场景）：

```bash
# 明文联调（deploybench 等传统 bench；板 prov 为 tcp locator）
~/project/tools/zenohd --listen tcp/0.0.0.0:9955 --no-multicast-scouting \
  --cfg='scouting/gossip/enabled:false'
# TLS mTLS 双监听（P1-2 TLS E2E 态；证书经 tools/board/gen_tls_certs.sh 生成）
~/project/tools/zenohd --listen tls/0.0.0.0:9956 --listen tcp/0.0.0.0:9957 \
  --no-multicast-scouting --cfg='scouting/gossip/enabled:false' \
  --cfg='transport/link/tls/enable_mtls:true' \
  --cfg='transport/link/tls/listen_private_key:"~/project/tls/router.key"' \
  --cfg='transport/link/tls/listen_certificate:"~/project/tls/router.crt"' \
  --cfg='transport/link/tls/root_ca_certificate:"~/project/tls/ca.crt"'
```

**串口纪律**（教训 36-39 摘要）：烧录前必停串口持有器（`fuser /dev/ttyACM0`
按 PID 杀——pkill/pgrep -f 字面量与命令同体 = 自杀）；持有器打开串口后
DTR/RTS 双 deassert（下载电路毛刺域）；烧录端口显式指定（`ESPPORT=` 或
`--port`），真 prov 烧录置于镜像定型之后（教训 39：prov-write-test 兜底
会静默覆盖）。

## 清单

### E2E 验收驱动（zenoh 无线部署 → 真机判据）

| 脚本 | 用途 | 对应登记 |
|---|---|---|
| `e2e_tls.py` | P1-2 TLS E2E：经 9957 TCP 驱动部署，板走 9956 mTLS，安装闸 timeout 路判据 | docs/evidence/p1_tls_e2e_console.txt |
| `e2e_deploy_smoke.py` | atk 板部署冒烟（deploy→激活→判据） | K1 批 |
| `e2e_trust_chain.py` | 信任链 T0-T7（注册/多根/防回滚/撤回） | docs/evidence/ak3_trust_*.txt |
| `e2e_resource_push.py` | net+SD 资源推送 E2E（needs.resources + sha256 读回） | AK4 RES PASS |
| `e2e_large_deploy.py` | large 档 206KB wasm 全链 | AK5 LARGE PASS |
| `e2e_ln_fanout.py` | 双板逻辑节点扇出/聚合（需第二板） | AK6 LN PASS |
| `e2e_contract_spotcheck.py` | 合同泛化 spot check（caps 预检 + 部署链） | docs/evidence/cg7_deploy_console.txt |
| `demo_multiapp.py` | 多 APP 并行部署（双 slot） | docs/evidence/ma_PASS_console.txt |
| `nvd_acceptance.py` | 新机全链：真 LLM 生成 → 部署 → 判据（环境验收金标准） | docs/evidence/nvd_PASS_console_native.txt |
| `demo_llm_creation.py` | 真 LLM 多文件工程创作循环 | docs/evidence/ag7_console.txt |
| `demo_pdm.py` / `demo_threads.py` | PDM 音频 / wasm 多线程 demo | docs/evidence/dp_PASS_console.txt 等 |

### 支撑工具

| 脚本 | 用途 |
|---|---|
| `gen_prov_image.py` | prov 分区映像生成（v1/v2 字段族）——正式烧录建议 `agent/tools/flash_prov.py`（esptool 直写 + TLS 三元组 + lconf） |
| `gen_tls_certs.sh` | TLS 证书链生成（EC P-256 CA/router〔SAN IP〕/device；设备私钥 PKCS8 DER——mbedtls 4.x 解析链定案）→ `~/project/tls/`（600 仓外） |
| `console_smoke.py` | 串口冒烟（板启动行捕获） |
| `console_holder.py` | 韧性串口持有器（USJ 断连自愈；CH343 用法 = 复制后改端口 + 打开后 DTR/RTS 双 deassert + EN 脉冲，见 dev-env 教训 38/39） |
| `fetch_blobs.py` | hal_espressif WiFi blobs 补齐（代理通道下 west blobs fetch 的替代手法） |
| `dav_recv.py` / `pdm_recv.py` | PC 侧帧/音频消费端（分片 crc 重组 → .rgb565/.pcm/.wav 落盘） |
| `ota_client.py` | MCUmgr SMP OTA 客户端（上传 → swap → confirm） |

## 典型编排（TLS E2E 全链，新机器冒烟可照抄）

```bash
source ~/project/logs/env.sh
# ① 证书（首次）+ zenohd TLS 双监听
tools/board/gen_tls_certs.sh && <zenohd TLS 命令见上>
# ② 板镜像（committed 配置；凭据注入 + 路由 9957）
west build -p always -b dnesp32s3b/esp32s3/procpu firmware/tests/atkbench \
  -d ~/project/builds/ak_build -- -DATKBENCH_WIFI_SSID="$TS_WIFI_SSID" \
  -DATKBENCH_WIFI_PASSWORD="$TS_WIFI_PASSWORD" -DATKBENCH_ROUTER='"tcp/192.168.2.91:9957"'
# ③ 烧录（显式端口；持有器已停）
ESPPORT=/dev/ttyACM0 west flash -d ~/project/builds/ak_build
# ④ prov 15 对 TLS（编排末步——教训 39）
python3 agent/tools/flash_prov.py --node akn --cube akc --router 'tls/192.168.2.91:9956' \
  --pub0 agent/tests/fixtures/ts-test-root.pub --lconf 1 \
  --tls-ca ~/project/tls/ca.der --tls-cert ~/project/tls/device_cert.der \
  --tls-key ~/project/tls/device_key.der --port /dev/ttyACM0 --offset 0x5f0000
# ⑤ 复位 → console 判据：AKa0 rc=0 / AK7 SD mounted / AKU1 rc=0 /
#    [ts-net] tls: mTLS 注入 / CONNECTED loc=tls/…:9956
# ⑥ 驱动验收
python3 tools/board/e2e_tls.py
```
