#!/usr/bin/env python3
"""r15 MCU 真云闸驱动：复位→等 CONFIG_WAIT→串口注入凭据（P4C2：
ssid/psk/host/api_key/model）→抓 [hw-cloud] 证据行。凭据只经
/home/emb/project/.cloud-credentials.json 读取（不入仓库/日志；
printk 只留长度）。host 从 base_url 解析（去 scheme 与路径——
同 F-2 host 版 parse_base_url 语义）。"""

import json
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

import serial

CRED = Path("/home/emb/project/.cloud-credentials.json")
PORT = "/dev/ttyACM0"
BAUD = 115200


def main() -> int:
    cred = json.loads(CRED.read_text())
    ssid = cred["wifi_ssid"].encode()
    psk = cred["wifi_password"].encode()
    u = urlsplit(cred["llm_base_url"])
    host = (u.netloc + u.path.rstrip("/")).encode()  # host/path 形式
    key = cred["llm_api_key"].encode()
    model = cred["allowed_models"][0].encode()
    assert 0 < len(ssid) <= 32 and 8 <= len(psk) <= 63, "wifi shape"
    assert 0 < len(host) <= 63, "host shape"
    assert 20 <= len(key) <= 127, "key shape"
    assert 0 < len(model) <= 47, "model shape"
    s = serial.Serial(PORT, BAUD, timeout=1)
    s.dtr = False
    s.rts = False
    time.sleep(0.1)
    s.rts = True
    time.sleep(0.1)
    s.rts = False  # EN 硬复位
    t0 = time.time()
    buf = b""
    injected = False
    while time.time() - t0 < 150:
        buf += s.read(4096)
        if not injected and b"CONFIG_WAIT" in buf:
            time.sleep(0.3)
            s.read(4096)
            s.write(
                b"P4C2"
                + bytes([len(ssid), len(psk), len(host), len(key), len(model)])
                + ssid
                + psk
                + host
                + key
                + model
            )
            injected = True
            print(
                f"[inject] sent ssid={len(ssid)}B psk={len(psk)}B "
                f"host={len(host)}B key={len(key)}B model={len(model)}B"
            )
        if (
            b"PASS: MCU real-cloud loop closed" in buf
            or b"FAIL" in buf
            or b"fatal" in buf.lower()
        ):
            time.sleep(1.0)
            buf += s.read(4096)
            break
    s.close()
    out = buf.decode("utf-8", "replace")
    sys.stdout.write(out)
    ok = "PASS: MCU real-cloud loop closed" in out and "FAIL" not in out
    print(f"[gate] {'ALL PASS' if ok else 'INCOMPLETE'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
