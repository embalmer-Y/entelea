#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# gen_tls_certs.sh — zenoh TLS/mTLS 证书体系生成（P1-2 · DEC-63②/Q-47②）。
#
# 自建离线 CA + 路由器（服务器）证书 + 设备（客户端）证书，全部 EC P-256：
#   ca.{key,crt,der}        CA 私钥/证书（owner 持有——CA 私钥永不入仓、不出本机）
#   router.{key,crt,der}    zenohd TLS 监听证书（SAN 含本机 IP——设备侧名称校验
#                           VERIFY_REQUIRED 要求 SAN 匹配 locator 主机）
#   device.{key,crt,der}    设备客户端证书（prov tls_cert/tls_key 消费）
# PEM 供 zenohd；DER 供 flash_prov.py --tls-*（prov 15 键三元组）。
#
# 用法：tools/board/gen_tls_certs.sh [outdir] [router_ip]
#   outdir 缺省 ~/project/tls（仓外纪律——密钥 600 永不入仓）
#   router_ip 缺省 192.168.2.91（本机 wlan/eth 地址——SAN 写入）
# 幂等：CA 存在即复用（撤销/轮换 = 删 ca.* 后重跑 = 全量重签）；设备证书
#   增量重签 = 删 device.* 后重跑。bench 期单张共享设备证书（DEC-63②）；
#   量产 per-device 由 CA 签发（生产化批）。
set -euo pipefail

DIR="${1:-$HOME/project/tls}"
IP="${2:-192.168.2.91}"
mkdir -p "$DIR"
cd "$DIR"
umask 077

if [[ ! -f ca.key ]]; then
	openssl ecparam -name prime256v1 -genkey -noout -out ca.key
	openssl req -x509 -new -key ca.key -sha256 -days 3650 \
		-subj "/CN=Tessera Bench CA" -out ca.crt
	echo "CA 生成（3650 天）"
fi

# 路由器（服务器）证书：SAN = IP（设备侧 VERIFY_REQUIRED 名称校验面）
openssl ecparam -name prime256v1 -genkey -noout -out router.key
openssl req -new -key router.key -subj "/CN=ts-router" -out router.csr
openssl x509 -req -in router.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
	-sha256 -days 825 -out router.crt \
	-extfile <(printf 'subjectAltName=IP:%s\nbasicConstraints=CA:FALSE\n' "$IP")

# 设备（客户端）证书（bench 期共享单张——DEC-63②）
openssl ecparam -name prime256v1 -genkey -noout -out device.key
openssl req -new -key device.key -subj "/CN=tessera-device" -out device.csr
openssl x509 -req -in device.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
	-sha256 -days 825 -out device.crt \
	-extfile <(printf 'basicConstraints=CA:FALSE\n')

# DER 形态（prov 三元组消费；router.der 不入 prov——仅 zenohd PEM）。
# 设备私钥 = PKCS8 DER（mbedtls 4.x 解析链第一优先——SEC1 形态在 4.x
# PSA 后端实测 -0x3D00 拒收，DEC-63③ E2E 定案；CA/证书 = X.509 DER）
openssl x509 -in ca.crt -outform DER -out ca.der
openssl x509 -in device.crt -outform DER -out device_cert.der
openssl pkcs8 -topk8 -nocrypt -in device.key -outform DER -out device_key.der

chmod 600 "$DIR"/*.key "$DIR"/*.der "$DIR"/*.crt "$DIR"/ca.srl 2>/dev/null || true
echo "证书就绪 @ $DIR："
ls -l "$DIR" | awk 'NR>1 {printf "  %-18s %6dB\n", $NF, $5}'
echo "prov 消费（DER）：ca.der + device_cert.der + device_key.der"
