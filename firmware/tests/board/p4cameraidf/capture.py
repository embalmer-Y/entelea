import argparse
import base64
import os
import re
import subprocess
import time
import zlib
from pathlib import Path

import serial
from PIL import Image, ImageStat

TARGET = Path("/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B79068007-if00")
parser = argparse.ArgumentParser(
    description="Capture a private SC2336 diagnostic preview from board 68007"
)
parser.add_argument(
    "--output", type=Path, required=True, help="New PNG path outside the repository"
)
OUTPUT = parser.parse_args().output
HEADER = re.compile(
    rb"P4CAM FRAME_BEGIN width=(\d+) height=(\d+) bytes=(\d+) "
    rb"adler32=([0-9a-f]{8}) format=RGB565"
)

if OUTPUT.suffix.lower() != ".png":
    raise ValueError("Output must be a PNG path")
if OUTPUT.resolve().is_relative_to(Path(__file__).resolve().parents[4]):
    raise ValueError("Camera previews must be stored outside the repository")
if OUTPUT.exists():
    raise RuntimeError("Refusing to overwrite an existing camera preview")
if not TARGET.exists() or TARGET.resolve().name != "ttyACM0":
    raise RuntimeError("Confirmed board serial target changed")
if subprocess.run(["fuser", "-s", str(TARGET)], check=False).returncode != 1:
    raise RuntimeError("Serial port is occupied or fuser failed")

os.umask(0o077)
port = serial.Serial(port=None, baudrate=115200, timeout=1, exclusive=True)
port.dtr = False
port.rts = False
port.port = str(TARGET)
port.open()
try:
    port.reset_input_buffer()
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    deadline = time.monotonic() + 180
    metadata = None
    image = bytearray()
    completed = False
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if not line:
            continue
        if metadata is None:
            match = HEADER.fullmatch(line)
            if match:
                width, height, size = map(int, match.group(1, 2, 3))
                checksum = int(match.group(4), 16)
                if not (1 <= width <= 960 and 1 <= height <= 540 and size == width * height * 2):
                    raise RuntimeError("Unsupported image dimensions or byte count")
                metadata = (width, height, size, checksum)
                print(line.decode(), flush=True)
            else:
                text = line.decode(errors="replace")
                print(text, flush=True)
                if "P4CAM FAIL" in text:
                    raise RuntimeError("Board reported an acquisition error")
        elif len(image) < metadata[2]:
            if len(line) > 1024:
                raise RuntimeError("Oversized base64 line")
            image.extend(base64.b64decode(line, validate=True))
            if len(image) > metadata[2]:
                raise RuntimeError("Frame exceeded announced byte count")
        else:
            expected_end = f"P4CAM FRAME_END adler32={metadata[3]:08x}".encode()
            if line != expected_end:
                raise RuntimeError(f"Unexpected frame trailer: {line[:100]!r}")
            print(line.decode(), flush=True)
            while time.monotonic() < deadline:
                status = port.readline().strip()
                if not status:
                    continue
                print(status.decode(errors="replace"), flush=True)
                if b"P4CAM FAIL" in status:
                    raise RuntimeError("Board reported an acquisition cleanup error")
                if status == b"P4CAM END":
                    completed = True
                    break
            break
    if metadata is None or not completed:
        raise TimeoutError("No complete frame received before deadline")
    width, height, size, checksum = metadata
    actual = zlib.adler32(image) & 0xFFFFFFFF
    if len(image) != size or actual != checksum:
        raise RuntimeError(f"Frame integrity failure: length={len(image)} adler32={actual:08x}")

    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        value = image[2 * i] | image[2 * i + 1] << 8
        pixels[3 * i] = ((value >> 11) & 31) * 255 // 31
        pixels[3 * i + 1] = ((value >> 5) & 63) * 255 // 63
        pixels[3 * i + 2] = (value & 31) * 255 // 31
    preview = Image.frombytes("RGB", (width, height), bytes(pixels))
    with OUTPUT.open("xb") as file:
        preview.save(file, format="PNG")
    print(
        f"VERIFIED bytes={size} adler32={actual:08x} png={OUTPUT} "
        f"extrema={preview.getextrema()} stddev={ImageStat.Stat(preview).stddev}",
        flush=True,
    )
finally:
    port.close()
