# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""console 冒烟（迁移重建版）：RTS 复位 + 抓取启动全程。

用法：python console_smoke.py [device] [seconds]
设备默认 /dev/ttyACM0，窗口默认 12s。判据：出现 BOOT/step8 等 boot 标记行。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
WINDOW = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

ser = serial.Serial(
    port=PORT,
    baudrate=115200,
    bytesize=serial.EIGHTBITS,
    parity=serial.PARITY_NONE,
    stopbits=serial.STOPBITS_ONE,
    timeout=1.0,
)
# RTS 复位（xiao_esp32s3 USB-JTAG：DTR/RTS 组合触发硬复位）
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
time.sleep(0.05)
ser.setDTR(True)

ser.reset_input_buffer()
print(f"=== console capture {PORT} {WINDOW}s ===")
deadline = time.time() + WINDOW
lines = []
while time.time() < deadline:
    raw = ser.readline()
    if not raw:
        continue
    try:
        line = raw.decode("utf-8", errors="replace").rstrip()
    except Exception:
        continue
    print(line, flush=True)
    lines.append(line)
ser.close()

markers = [l for l in lines if any(k in l for k in ("BOOT", "boot", "step", "Tessera", "ts_", "APP"))]
print(f"=== 共 {len(lines)} 行，含启动标记 {len(markers)} 行 ===")
sys.exit(0 if lines else 1)
