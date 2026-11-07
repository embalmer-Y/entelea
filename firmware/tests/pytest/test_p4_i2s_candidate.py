# SPDX-License-Identifier: Apache-2.0
"""Verify the isolated I2S candidate rejects an unexpected GDMA source."""

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / "firmware/tests/board/p4i2sbench/gdma_candidate.cmake"


def test_candidate_rejects_modified_gdma(tmp_path):
    zephyr = tmp_path / "zephyr"
    driver = zephyr / "drivers/dma/dma_esp32_gdma.c"
    driver.parent.mkdir(parents=True)
    driver.write_text("/* modified, not the pinned source */\n", encoding="utf-8")
    (tmp_path / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.28)\n"
        "project(candidate_guard C)\n"
        f'set(ZEPHYR_BASE "{zephyr}")\n'
        f'add_library(drivers__dma STATIC "{driver}")\n'
        f'include("{HELPER}")\n',
        encoding="utf-8",
    )
    result = subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(tmp_path / "build")],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert result.returncode != 0
    assert "requires the unmodified pinned GDMA source" in result.stderr


def test_lifecycle_rejects_16_bit_slots(tmp_path):
    zephyr_package = tmp_path / "zephyr-package"
    zephyr_package.mkdir()
    (zephyr_package / "ZephyrConfig.cmake").write_text(
        "add_library(app INTERFACE)\n", encoding="utf-8"
    )
    result = subprocess.run(
        [
            "cmake", "-S", str(HELPER.parent), "-B", str(tmp_path / "build"),
            f"-DZephyr_DIR={zephyr_package}",
            "-DP04_I2S_LIFECYCLE_TEST=ON",
            "-DP04_I2S_WORD32=OFF",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert result.returncode != 0
    assert "lifecycle diagnostic requires 32-bit slots" in result.stderr
