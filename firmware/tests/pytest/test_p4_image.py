# SPDX-License-Identifier: Apache-2.0
"""Exercise the real P4 image producer and its board CMake hook, without hardware."""

import hashlib
import runpy
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
PRODUCER = REPO / "scripts/prepare_p4_image.py"
MODULE = runpy.run_path(str(PRODUCER))
terminate = MODULE["terminated_image"]
prepare = MODULE["prepare_image"]
MARKER = b"\xff" * 8


def image(rom_size=4, padding_segments=1, padding_size=32):
    header = bytearray(24)
    header[0:2] = bytes([0xE9, 1])
    struct.pack_into("<H", header, 12, 18)
    ram = b"\x01\x02\x03\x04"
    result = header + struct.pack("<II", 0x4FF10000, len(ram)) + ram
    checksum = 0xEF
    for byte in ram:
        checksum ^= byte
    result += bytes(((len(result) + 16) & ~15) - len(result) - 1)
    result += bytes([checksum])
    result += struct.pack("<II", 0, padding_size) + bytes(padding_size)
    result += struct.pack("<II", 0, 0) * (padding_segments - 1)
    result += struct.pack("<II", 0x40000000, rom_size) + b"\xff" * rom_size
    return bytes(result)


def expected_image(original):
    result = bytearray(original)
    result[23] = 1
    result[48:80] = hashlib.sha256(result[:48]).digest()
    struct.pack_into("<II", result, 80, 0, struct.unpack_from("<I", original, 52)[0] - 32)
    return bytes(result) + MARKER


@pytest.mark.parametrize("rom_size", [4, 8, 4032, 4096])
def test_termination_does_not_confuse_segment_data_with_marker(rom_size):
    original = image(rom_size)
    result = terminate(original, 8192)
    assert result == expected_image(original)
    assert result[88:-8] == original[88:]
    assert len(result) - len(original) == 8
    assert terminate(result, len(result)) == result


def test_exact_sector_boundary_and_zero_sized_padding_are_supported():
    original = image(4000)
    assert len(original) == 4096
    assert terminate(original, 4104) == expected_image(original)


@pytest.mark.parametrize("padding_size", [32, 36, 64, 752])
def test_digest_shrinks_padding_without_moving_rom_segments(padding_size):
    original = bytearray(image(padding_size=padding_size))
    if padding_size > 32:
        original[88] = 0x5A
    result = terminate(bytes(original), 8192)
    assert result == expected_image(bytes(original))
    assert result[88:-8] == original[88:]
    assert len(result) == len(original) + 8


@pytest.mark.parametrize("limit", [0, -1, 4095, 4096, 4103])
def test_marker_must_fit_inside_firmware_range(limit):
    with pytest.raises(ValueError, match="firmware range"):
        terminate(image(4000), limit)


@pytest.mark.parametrize("cut", [0, 23, 27, 35, 47, 51, 59, 63])
def test_truncated_images_are_rejected(cut):
    with pytest.raises(ValueError):
        terminate(image()[:cut], 8192)


@pytest.mark.parametrize(
    ("offset", "replacement", "message"),
    [
        (0, b"\x00", "header"),
        (1, b"\x00", "RAM segment count"),
        (1, b"\x10", "RAM segment count"),
        (12, b"\x00\x00", "ESP32-P4"),
        (23, b"\x02", "digest flag"),
        (24, struct.pack("<I", 0x40000000), "RAM segment range"),
        (28, struct.pack("<I", 3), "Unaligned"),
        (47, b"\x00", "RAM checksum"),
        (88, struct.pack("<I", 0xDEADBEEF), "ROM segment range"),
        (92, struct.pack("<I", 5), "Unaligned"),
    ],
)
def test_malformed_images_are_rejected(offset, replacement, message):
    original = bytearray(image())
    original[offset:offset + len(replacement)] = replacement
    with pytest.raises(ValueError, match=message):
        terminate(bytes(original), 8192)


def test_loader_segment_limit_reserves_one_entry_for_marker():
    assert terminate(image(padding_segments=13), 8192).endswith(MARKER)
    with pytest.raises(ValueError, match="loader segment limit"):
        terminate(image(padding_segments=14), 8192)


def test_bytes_after_existing_marker_are_not_silently_accepted():
    with pytest.raises(ValueError, match="after the end marker"):
        terminate(image() + MARKER + bytes(8), 8192)


def test_existing_unsigned_marker_is_preserved_when_adding_digest():
    original = image()
    assert terminate(original + MARKER, 8192) == expected_image(original)


def test_existing_valid_digest_gets_a_terminator():
    expected = expected_image(image())
    assert terminate(expected[:-8], 8192) == expected


@pytest.mark.parametrize("offset", [48, 64, 79])
def test_existing_digest_corruption_is_rejected(offset):
    corrupted = bytearray(expected_image(image()))
    corrupted[offset] ^= 1
    with pytest.raises(ValueError, match="Invalid RAM digest"):
        terminate(bytes(corrupted), 8192)


def test_ram_corruption_with_unchanged_xor_is_rejected_by_digest():
    corrupted = bytearray(expected_image(image()))
    corrupted[32], corrupted[33] = corrupted[33], corrupted[32]
    with pytest.raises(ValueError, match="Invalid RAM digest"):
        terminate(bytes(corrupted), 8192)


def test_insufficient_or_nonzero_padding_is_rejected():
    short_padding = bytearray(image())
    struct.pack_into("<I", short_padding, 52, 28)
    with pytest.raises(ValueError, match="Insufficient initial padding"):
        terminate(bytes(short_padding), 8192)
    nonzero_padding = bytearray(image())
    nonzero_padding[56] = 1
    with pytest.raises(ValueError, match="nonzero"):
        terminate(bytes(nonzero_padding), 8192)


def test_truncated_digest_is_rejected():
    signed = expected_image(image())
    with pytest.raises(ValueError, match="Truncated RAM digest"):
        terminate(signed[:64], 8192)


def test_file_update_is_idempotent_and_preserves_permissions(tmp_path):
    binary = tmp_path / "zephyr.bin"
    original = image()
    binary.write_bytes(original)
    binary.chmod(0o640)
    prepare(binary, 8192)
    assert binary.read_bytes() == expected_image(original)
    assert binary.stat().st_mode & 0o777 == 0o640
    modified = binary.stat().st_mtime_ns
    prepare(binary, 8192)
    assert binary.stat().st_mtime_ns == modified
    assert list(tmp_path.iterdir()) == [binary]


def test_failed_validation_leaves_original_file_untouched(tmp_path):
    binary = tmp_path / "zephyr.bin"
    original = image()[:-1]
    binary.write_bytes(original)
    with pytest.raises(ValueError, match="truncated segment data"):
        prepare(binary, 8192)
    assert binary.read_bytes() == original
    assert list(tmp_path.iterdir()) == [binary]


@pytest.mark.parametrize("simple_boot", [True, False])
def test_actual_board_hook_runs_after_image_generation_and_tracks_producer(tmp_path, simple_boot):
    board = tmp_path / "firmware/boards/osptek/esp32p4c5_core_board"
    board.mkdir(parents=True)
    shutil.copyfile(
        REPO / "firmware/boards/osptek/esp32p4c5_core_board/image.cmake", board / "image.cmake"
    )
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    shutil.copyfile(PRODUCER, scripts / PRODUCER.name)
    (tmp_path / "input.bin").write_bytes(image())
    (tmp_path / "main.c").write_text("int main(void) { return 0; }\n")
    (tmp_path / "CMakeLists.txt").write_text(
        f"""\
cmake_minimum_required(VERSION 3.20)
project(image_hook C)
set(ZEPHYR_BASE "{tmp_path}/zephyr")
set(PROJECT_BINARY_DIR "${{CMAKE_BINARY_DIR}}")
set(PYTHON_EXECUTABLE "{sys.executable}")
set(KERNEL_BIN_NAME zephyr.bin)
set(CONFIG_SOC_ESP32P4 ON)
set(CONFIG_ESP_SIMPLE_BOOT {"ON" if simple_boot else "OFF"})
set(CONFIG_BUILD_OUTPUT_BIN ON)
function(dt_nodelabel output)
  set(${{output}} "/boot" PARENT_SCOPE)
endfunction()
function(dt_chosen output)
  set(${{output}} "/code" PARENT_SCOPE)
endfunction()
function(dt_reg_addr output)
  if(ARGV2 STREQUAL "/boot")
    set(${{output}} 8192 PARENT_SCOPE)
  else()
    set(${{output}} 131072 PARENT_SCOPE)
  endif()
endfunction()
function(dt_reg_size output)
  set(${{output}} 8126464 PARENT_SCOPE)
endfunction()
add_library(zephyr_interface INTERFACE)
include("{board}/image.cmake")
add_executable(zephyr_final main.c)
target_link_libraries(zephyr_final PRIVATE zephyr_interface)
get_property(commands GLOBAL PROPERTY extra_post_build_commands)
add_custom_command(TARGET zephyr_final POST_BUILD
  COMMAND "${{CMAKE_COMMAND}}" -E copy "${{CMAKE_SOURCE_DIR}}/input.bin"
    "${{CMAKE_BINARY_DIR}}/zephyr.bin"
  ${{commands}})
"""
    )
    build = tmp_path / "build"
    subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build), "-G", "Ninja"],
        check=True, capture_output=True, text=True, timeout=30,
    )
    command = ["cmake", "--build", str(build), "--target", "zephyr_final"]
    first = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert first.returncode == 0, first.stdout + first.stderr
    expected = expected_image(image()) if simple_boot else image()
    assert (build / "zephyr.bin").read_bytes() == expected
    second = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert second.returncode == 0, second.stdout + second.stderr
    assert "image finalized" not in second.stdout
    with (scripts / PRODUCER.name).open("a") as output:
        output.write("\n")
    third = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert third.returncode == 0, third.stdout + third.stderr
    assert ("image finalized" in third.stdout) is simple_boot
    assert (build / "zephyr.bin").read_bytes() == expected
