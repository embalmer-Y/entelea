"""Verify that the board RAM gate fails closed on malformed linker maps."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "check_board_ram.py"
MAP = """\
Memory Configuration

Name             Origin             Length             Attributes
sram0_0_seg      0x4ff20000         0x0009cfc0         rw

Linker script and memory map

.iram0.text     0x4ff20000     0x100
 .nested.input  0x4ff20020      0x80 object.o
sw_isr_table
                0x4ff20100      0x20
.dram0.data     0x4ff20120      0x10
.dram0.noinit   0x4ff20130      0x20
.dram0.bss      0x4ff20150      0x40
.last_ram_section
                0x4ff20190      0x10
OUTPUT(zephyr/zephyr.elf elf32-littleriscv)
"""


class BoardRamTests(unittest.TestCase):
    def run_checker(self, content, limit=552960):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "image.map"
            path.write_text(content)
            return subprocess.run(
                [sys.executable, str(CHECKER), str(path), "--limit", str(limit)],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_counts_only_output_sections_including_split_and_non_dot_names(self):
        result = self.run_checker(MAP, limit=0x1A0)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("416 / limit 416", result.stdout)
        self.assertNotIn(".nested.input", result.stdout)
        self.assertIn("sw_isr_table", result.stdout)
        self.assertIn(".last_ram_section", result.stdout)

    def test_split_output_sections_cannot_bypass_limit(self):
        result = self.run_checker(
            MAP.replace(" .nested.input  0x4ff20020      0x80 object.o\n", ""),
            limit=400,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("416 / limit 400", result.stdout)

    def test_empty_and_incomplete_maps_are_rejected(self):
        for content in (
            "",
            MAP.replace("OUTPUT(zephyr/zephyr.elf elf32-littleriscv)\n", ""),
            MAP.replace("OUTPUT(zephyr/zephyr.elf elf32-littleriscv)", "OUTPUT("),
            MAP.replace(".dram0.bss      0x4ff20150      0x40\n", ""),
            MAP.replace("sram0_0_seg", "different_segment"),
        ):
            with self.subTest(content=content):
                result = self.run_checker(content)
                self.assertNotEqual(
                    result.returncode, 0, result.stdout + result.stderr
                )
                self.assertIn("FAIL", result.stderr)

    def test_overlapping_output_sections_are_rejected(self):
        content = MAP.replace(
            "OUTPUT(", ".overlap        0x4ff20100      0x80\nOUTPUT("
        )
        result = self.run_checker(content)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL", result.stderr)

    def test_section_straddling_hp_boundary_is_rejected(self):
        content = MAP.replace(
            "OUTPUT(", ".straddle       0x4ffbfff0     0x100\nOUTPUT("
        )
        result = self.run_checker(content)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL", result.stderr)


if __name__ == "__main__":
    unittest.main()
