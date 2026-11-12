#!/usr/bin/env python3
"""F-1 真网闸驱动：烧录→复位→等 CONFIG_WAIT→串口注入凭据（P4C1）→
抓 [hw-wifi] 证据行。凭据只经 /home/emb/project/.cloud-credentials.json
读取（不入仓库/日志；printk 只留长度）。"""

import json
import sys
import time
from pathlib import Path

import serial

CRED = Path("/home/emb/project/.cloud-credentials.json")
PORT = "/dev/ttyACM0"
BAUD = 115200


def main() -> int:
    cred = json.loads(CRED.read_text())
    ssid, psk = cred["wifi_ssid"].encode(), cred["wifi_password"].encode()
    assert 0 < len(ssid) <= 32 and 8 <= len(psk) <= 63, "credential shape"
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
    while time.time() - t0 < 90:
        buf += s.read(4096)
        if not injected and b"CONFIG_WAIT" in buf:
            time.sleep(0.3)
            s.read(4096)
            s.write(b"P4C1" + bytes([len(ssid), len(psk)]) + ssid + psk)
            injected = True
            print(f"[inject] credentials sent (ssid={len(ssid)}B psk={len(psk)}B)")
        if (
            b"PASS: wifi/netmon loop closed" in buf
            or b"FAIL" in buf
            or b"fatal" in buf.lower()
        ):
            time.sleep(1.0)
            buf += s.read(4096)
            break
    s.close()
    out = buf.decode("utf-8", "replace")
    print(out)
    ok = "PASS: wifi/netmon loop closed" in out and "FAIL" not in out
    print("[gate-wifi]", "ALL PASS" if ok else "FAIL")
    Path("/tmp/gate-wifi-run.log").write_text(out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
