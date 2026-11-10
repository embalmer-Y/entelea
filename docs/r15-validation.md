# r15 验证记录：MCU 真云闭环（A-1）+ board 资源表建立 + D12 基线

日期：2026-11-10。owner 授权：A-1 MCU 侧真云 → 基线测试；内存实测与
分配汇总入 board 专属表格（裁决等测试数据后再定）。

## 1. A-1 MCU 真云闸（终态 ALL PASS）

**链路**：P4C2 串口凭据注入（ssid/psk/host+path/key/model，60s 窗，
不入镜像不入仓库，用后 zeroize）→ WiFi 关联 → DHCP（192.168.2.120）
→ dep NET good×2 → **板上 TLS1.3**（mbedtls 4.1.1 Zephyr 模块，pin
同源 dependencies.json）→ 真模型 chat → content+usage 断言 →
dep MODEL UP → 断链 → chat 准入拒（E_STATE，touched 不动）→
复连（DNS 重试）→ 第二轮 chat → PASS 常驻。证据
build/r15-evidence/mcu-cloud-gate-ALLPASS.log。

**判据行**（[hw-cloud]，11/11 PASS）：
- 连网+凭据+DHCP+dep NET UP ×2 轮
- real TLS chat ×2：reply=ok usage=24+80 / reply2=ok usage=24+84
  （模型两次回字面 ok；断言=非空+usage+touched，不强绑字面——B-2）
- dep MODEL UP；断链拒 rc=-4(E_STATE)+哨兵不动；两轮闭环 ALL PASS

## 2. 组件（新）

- **platform/zephyr/https_chat.{c,h}**：与 platform/linux 版同源合同
  （值感知 JSON/超时量子/首行状态/截断显式）；差异面=Zephyr POSIX
  socket+getaddrinfo、CA 嵌入、BIO 自管 fd、k_uptime。body 8K
  （max_tokens 256 响应界）。
- **platform/zephyr/ca_roots.c**：嵌入 DigiCert Global Root G2 + ISRG
  X1（选根依据=openssl 证书链探测：对端 RSA 2048、链根 G1/G2 交叉）。
- **apps/hw_cloud_chat**：闸本体（P4C2 协议=hw_wifi_netmon 复用面+
  host/api_key/model 字段；netmon MODEL dep 联动；准入面与 host 版
  同语义）。
- **forge/gate/cloud_inject_board.py**：注入驱动（base_url 拆
  host+path——F-2 parse_base_url 等价）。

## 3. 攻坚链（7 个真缺陷，全部真机实证修复；诚实记录）

1. iface up 判定误用 errno（应为返回值 -EALREADY）——首构建即抓。
2. PEM 解析面未开（X509 CRT PARSE 需 PEM_PARSE_C+BASE64_C）。
3. **TLS1.3 签名方案缺 rsa_pss_rsae**（X509_RSASSA_PSS_SUPPORT 未开
   → hello 只 offer pkcs1/ecdsa → 对端 RSA 证书无可用方案 →
   alert 40）。定位=debug level4 dump hello（sent 列表缺 804-806）
   + **openssl -sigalgs pkcs1-only 复现同 alert**（服务器侧事实）
   + 源码预设表条件（ssl_preset_default_sig_algs 的 PSS 项挂在
   X509_RSASSA_PSS_SUPPORT，非 PSA_WANT）。
4. **tcp_work 线程栈溢出**（默认栈在 TLS 记录重组路径=FATAL 2
   Stack overflow；HW_STACK_PROTECTION+THREAD_NAME 定案线程名）。
   注：此前多轮"乱跳型 CPU exception"（mepc 落数据区/ra 垃圾/
   崩点恒 rx_stack 区）同根因——栈溢出的破坏面随机。
5. esp_hosted RX/event 栈 4096→16384（驱动 Kconfig 自述 RPC 栈上
   解码须容最大帧；防御性，与 4 同轮）。
6. 状态行解析偏移错 1（手写版 vs host 版 sscanf 的空格跳过语义；
   dump sp 字节定案）+ 三位数字逐位校验补严。
7. path 前缀：base_url path=/api/v1（非 /v1）→ 405；注入侧拆分+
   设备侧 split（host[/prefix] 字段）。
8. 第二轮 DNS 首查未就绪（DHCP DNS 重注入时序）→ 3s 重试即过。

另：TF slot 在本闸 overlay 显式 disabled（闸不需要 TF；两 slot 共享
控制器的互扰假说最终未被证实为根因——真因是 4/5；保留 disabled 为
闸的显式裁剪并注释）。

## 4. board 资源表（新文档，owner 指示）

**docs/board-osptek-p4c5-resource-budget.md**：SoC 内存面、8 个板级
镜像 text/RAM 实测、栈/堆配置（含★踩坑定案值）、D12 运行时数据点、
配置教训沉淀。随批更新纪律已写入文档头。

## 5. D12 基线状态（数据备好，阈值待 owner 裁决）

- 已实测入库（board 表 §4）：内存/栈/堆全量、MWDT 8.8s、estop
  物理坍缩（定性）、TLS 往返成功性、WAMR 配额。
- **µs 级停止延迟定量需示波器**（defer——ISR 同步直写的结构性上界
  已记录）；唤醒时间=无睡眠面 defer；正式阈值表待 owner 拿本表裁决。

## 6. 复现

```sh
# 构建（4 模块：hal/nanopb/mbedtls/tf_psa_crypto）
python3 tools/build_zephyr.py --app platform/zephyr/apps/hw_cloud_chat \
  --board esp32p4c5_core_board/esp32p4/hpcore --board-root platform/zephyr \
  --zephyr-base ../entelea-legacy/.deps/zephyr \
  --python ../entelea-legacy/.venv/bin/python3 \
  --esptool-python ../entelea-legacy/.venv/bin/python3 \
  --sdk /home/emb/zephyr-sdk-1.0.1 --sdk-version 1.0.1 \
  --toolchain riscv64-zephyr-elf --gcc-version 14.3.0 \
  --module hal_espressif=../entelea-legacy/.deps/hal_espressif \
  --module nanopb=../entelea-legacy/.deps/nanopb \
  --module mbedtls=../entelea-legacy/.deps/mbedtls \
  --module tf_psa_crypto=../entelea-legacy/.deps/tf-psa-crypto \
  --cmake-arg=-DESP_IDF_PATH=/home/emb/project/entelea-legacy/.deps/hal_espressif \
  --build-dir build/r15-cloud --build
build/.toolbin/esptool --chip esp32p4 -p /dev/ttyACM0 -b 921600 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x2000 build/r15-cloud/zephyr/zephyr.bin
python3 /home/emb/project/workspace/forge/gate/cloud_inject_board.py
```

回归面：15 ztest native_sim + wdt_hardware + CTest 6/6×2 + host +
checkers + source-policy（71→74）——见批尾记录。板终态=minimal
常驻（已恢复）。

## 7. 云用量

本轮真模型调用 2 轮成功（212 token，台账已记）+ 不计费失败轮若干。
累计 ≈¥10.7/¥108。
