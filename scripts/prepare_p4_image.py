# SPDX-License-Identifier: Apache-2.0
"""Prepare the pinned unsigned ESP32-P4 Simple Boot RAM-only image."""

import argparse
import hashlib
import os
import struct
import tempfile
from pathlib import Path

HEADER_SIZE = 24
SEGMENT = struct.Struct("<II")
END_MARKER = b"\xff" * SEGMENT.size
DIGEST_SIZE = hashlib.sha256().digest_size
MAX_SEGMENTS = 16
RAM_RANGES = ((0x4FF00000, 0x4FFC0000), (0x50108000, 0x50110000))
ROM_RANGE = (0x40000000, 0x44000000)


def read_segment(data: bytes, offset: int) -> tuple[int, int, int]:
    if offset + SEGMENT.size > len(data):
        raise ValueError("Truncated segment header")
    address, length = SEGMENT.unpack_from(data, offset)
    start = offset + SEGMENT.size
    end = start + length
    if length & 3 or end > len(data):
        raise ValueError("Unaligned or truncated segment data")
    return address, start, end


def terminated_image(data: bytes, max_size: int) -> bytes:
    if max_size <= 0 or len(data) > max_size:
        raise ValueError("Image exceeds its device-tree firmware range")
    if len(data) < HEADER_SIZE or data[0] != 0xE9:
        raise ValueError("Invalid ESP image header")
    if struct.unpack_from("<H", data, 12)[0] != 18:
        raise ValueError("Only ESP32-P4 images are supported")
    if data[23] not in (0, 1):
        raise ValueError("Invalid RAM digest flag")
    count = data[1]
    if not 1 <= count < MAX_SEGMENTS:
        raise ValueError("Invalid RAM segment count")

    offset = HEADER_SIZE
    checksum = 0xEF
    for _ in range(count):
        address, start, end = read_segment(data, offset)
        if not any(low <= address < high and address + end - start <= high
                   for low, high in RAM_RANGES):
            raise ValueError("Invalid RAM segment range")
        for byte in data[start:end]:
            checksum ^= byte
        offset = end

    # RAM checksum occupies the last byte before the next 16-byte boundary.
    offset = (offset + 16) & ~15
    if offset > len(data) or data[offset - 1] != checksum:
        raise ValueError("Truncated or invalid RAM checksum")

    if data[23]:
        if offset + DIGEST_SIZE > len(data):
            raise ValueError("Truncated RAM digest")
        if data[offset:offset + DIGEST_SIZE] != hashlib.sha256(data[:offset]).digest():
            raise ValueError("Invalid RAM digest")
        offset += DIGEST_SIZE
    else:
        address, start, end = read_segment(data, offset)
        if address != 0 or end - start < DIGEST_SIZE:
            raise ValueError("Insufficient initial padding for RAM digest")
        if any(data[start:start + DIGEST_SIZE]):
            raise ValueError("Initial padding contains nonzero data")
        result = bytearray(data)
        result[23] = 1
        result[offset:offset + DIGEST_SIZE] = hashlib.sha256(result[:offset]).digest()
        SEGMENT.pack_into(result, offset + DIGEST_SIZE, 0, end - start - DIGEST_SIZE)
        data = bytes(result)
        offset += DIGEST_SIZE

    while offset < len(data):
        if data[offset:offset + SEGMENT.size] == END_MARKER:
            if offset + SEGMENT.size != len(data):
                raise ValueError("Unexpected bytes after the end marker")
            return data
        address, start, end = read_segment(data, offset)
        if address and not (ROM_RANGE[0] <= address < ROM_RANGE[1]
                            and address + end - start <= ROM_RANGE[1]):
            raise ValueError("Invalid ROM segment range")
        count += 1
        if count >= MAX_SEGMENTS:
            raise ValueError("No room for the end marker within the loader segment limit")
        offset = end

    if len(data) + len(END_MARKER) > max_size:
        raise ValueError("End marker exceeds the device-tree firmware range")
    return data + END_MARKER


def prepare_image(image: Path, max_size: int) -> None:
    original = image.read_bytes()
    result = terminated_image(original, max_size)
    if result == original:
        print("P4 Simple Boot image already terminated")
        return
    descriptor, temporary = tempfile.mkstemp(prefix=f".{image.name}.", dir=image.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(result)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, image.stat().st_mode & 0o777)
        os.replace(temporary, image)
    finally:
        Path(temporary).unlink(missing_ok=True)
    print("P4 Simple Boot image finalized with RAM digest and end marker")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--max-size", type=lambda value: int(value, 0), required=True)
    arguments = parser.parse_args()
    prepare_image(arguments.image, arguments.max_size)


if __name__ == "__main__":
    main()
