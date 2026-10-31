# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""韧性好串口持有器（USJ 断连自愈）——追加写 /tmp/ak3_console_live.txt。"""
import serial, time

LOG = "/tmp/ak3_console_live.txt"
while True:
    try:
        s = serial.Serial("/dev/ttyACM0", 115200, timeout=1)
        with open(LOG, "ab") as f:
            while True:
                d = s.read(4096)
                if d:
                    f.write(d)
                    f.flush()
    except Exception:
        time.sleep(1)
