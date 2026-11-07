import array
import concurrent.futures
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

TARGET = Path("/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B79068007-if00")
PCM = "/dev/snd/pcmC0D0p"
WINDOW = re.compile(r"P4MIC WINDOW i=(\d+) ch=(\d+) n=(\d+) ac=(\d+) f1000=(\d+) f2000=(\d+)")
RATE = 16000
AMPLITUDE = 2000
STAGES = ((0, 32768), (1000, 65536), (0, 32768), (2000, 65536), (0, 32768))

if not TARGET.exists() or TARGET.resolve().name != "ttyACM0":
    raise RuntimeError("Confirmed 68007 serial target changed")
for device in (str(TARGET), PCM):
    if subprocess.run(["fuser", "-s", device], check=False).returncode != 1:
        raise RuntimeError(f"Device occupied or fuser failed: {device}")

signal = array.array("h")
for frequency, frames in STAGES:
    for i in range(frames):
        sample = round(AMPLITUDE * math.sin(2 * math.pi * frequency * i / RATE))
        signal.extend((sample, sample))
if sys.byteorder != "little":
    signal.byteswap()
pcm = signal.tobytes()
del signal

port = serial.Serial(port=None, baudrate=115200, timeout=1, exclusive=True)
port.dtr = False
port.rts = False
port.port = str(TARGET)
os.umask(0o077)
port.open()
process = None
windows = {}
ready_at = None
executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)
try:
    port.reset_input_buffer()
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    deadline = time.monotonic() + 35
    complete = False
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        if "P4MIC" not in line:
            continue
        print(line, flush=True)
        if line.startswith("P4MIC FAIL"):
            raise RuntimeError("Board reported an audio diagnostic failure")
        if line.startswith("P4MIC READY"):
            if process is not None:
                raise RuntimeError("Unexpected second READY")
            ready_at = time.monotonic()
            process = subprocess.Popen(
                [
                    "aplay",
                    "-q",
                    "-D",
                    "plughw:CARD=PCH,DEV=0",
                    "-t",
                    "raw",
                    "-f",
                    "S16_LE",
                    "-c",
                    "2",
                    "-r",
                    str(RATE),
                    "--buffer-time=100000",
                    "--period-time=25000",
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            future = executor.submit(process.communicate, pcm, timeout=30)
            print(f"HOST OUTPUT_START elapsed={time.monotonic() - ready_at:.3f}s", flush=True)
        match = WINDOW.fullmatch(line)
        if match:
            index, channel, count, ac, f1000, f2000 = map(int, match.groups())
            if index >= 40 or channel >= 2 or count != 8192 or (index, channel) in windows:
                raise RuntimeError("Unexpected microphone statistics shape")
            windows[index, channel] = (ac, f1000, f2000)
        if line.startswith("P4MIC END"):
            if line != "P4MIC END rc=0 amp_rc=0 drop_rc=0":
                raise RuntimeError("Board could not cleanly stop audio")
            complete = True
            break
    if not complete or process is None or len(windows) != 80:
        raise RuntimeError(f"Incomplete trial: complete={complete} windows={len(windows)}")
    _, stderr = future.result(timeout=3)
    if process.returncode:
        raise RuntimeError(f"Computer audio playback failed: {stderr.decode(errors='replace')}")
    print(
        f"HOST OUTPUT_EXIT status={process.returncode} "
        f"total_elapsed={time.monotonic() - ready_at:.3f}s"
    )
    for ch in range(2):
        for label, indices in (
            ("before", range(1, 4)),
            ("1000", range(6, 11)),
            ("middle", range(14, 16)),
            ("2000", range(18, 23)),
            ("after", range(30, 35)),
        ):
            vals = [windows[index, ch] for index in indices]
            avg = tuple(sum(value[i] for value in vals) / len(vals) for i in range(3))
            print(
                f"HOST SUMMARY ch={ch} phase={label} ac={avg[0]:.1f} "
                f"f1000={avg[1]:.1f} f2000={avg[2]:.1f}"
            )
finally:
    if process is not None and process.poll() is None:
        process.kill()
        process.wait()
    executor.shutdown(wait=True, cancel_futures=True)
    port.close()
