"""Verify the three-tier total-memory gate and that it fails closed on bad maps."""

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "check_board_ram.py"
SRAM_WINDOW = 0x4FFC0000 - 0x4FF00000
BASE_DEMAND = 0x1A0  # the fixed output sections of board_map()


def board_map(psram=None, psram_section="", extra_hp=""):
    """Build a linker map; `extra_hp` adds SRAM output sections, one per line."""
    lines = [
        "Memory Configuration",
        "",
        "Name             Origin             Length             Attributes",
        "sram0_0_seg      0x4ff20000         0x0009cfc0         rw",
    ]
    if psram is not None:
        lines.append(f"ext_dram_seg     0x48000000         0x{psram:08x}         rw")
    lines += [
        "",
        "Linker script and memory map",
        "",
        ".iram0.text     0x4ff20000     0x100",
        " .nested.input  0x4ff20020      0x80 object.o",
        "sw_isr_table",
        "                0x4ff20100      0x20",
        ".dram0.data     0x4ff20120      0x10",
        ".dram0.noinit   0x4ff20130      0x20",
        ".dram0.bss      0x4ff20150      0x40",
        ".last_ram_section",
        "                0x4ff20190      0x10",
    ]
    lines += psram_section.splitlines()
    lines += extra_hp.splitlines()
    lines.append("OUTPUT(zephyr/zephyr.elf elf32-littleriscv)")
    return "\n".join(lines) + "\n"


def row(name, value, tail=""):
    """One line of the checker's table, in its own format.

    `value` may be "used / capacity"; `tail` a capacity or a label.
    """
    if isinstance(value, str) and " / " in value:
        value, capacity = value.split(" / ")
        tail = " / " + capacity
    elif tail and not tail.startswith(" "):
        tail = "  " + tail
    return f"{name:32s} {value:>8}{tail}"


def big(size, address=0x4FF20200):
    """One SRAM output section of `size` bytes that does not overlap the base."""
    return f".big            0x{address:08x}      0x{size:x}"


RODATA = 0x40000000
NO_ELF = object()


def elf_image(pools):
    """Build a 32-bit little-endian ELF holding the pool table Zephyr generates.

    `pools` is a sequence of (name, address, size); None builds an image that
    has no table at all, i.e. does not use the allocator.
    """
    names = bytearray(b"\x00")
    table = bytearray()
    for name, address, size in pools or ():
        name_at = len(names)
        names += name.encode() + b"\x00"
        table += struct.pack(
            "<IIII", RODATA + 16 * len(pools) + name_at, address, size, 0x10001
        )
    rodata = bytes(table) + bytes(names)
    strtab = b"\x00mem_attr_region\x00"
    symbols = bytes(16)
    if pools is not None:
        symbols += struct.pack("<IIIBBH", 1, RODATA, len(table), 0x11, 0, 1)
    rodata_at = 52
    symtab_at = rodata_at + len(rodata)
    strtab_at = symtab_at + len(symbols)
    section_table_at = strtab_at + len(strtab)
    section_names = b"\x00.rodata\x00.symtab\x00.strtab\x00"
    header = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    header += struct.pack(
        "<HHIIIIIHHHHHH",
        2,
        243,
        1,
        RODATA,
        0,
        section_table_at,
        0,
        52,
        0,
        0,
        40,
        5,
        0,
    )

    def section(name, kind, address, offset, size, link=0):
        return struct.pack("<10I", name, kind, 0, address, offset, size, link, 0, 4, 0)

    sections = b"".join(
        (
            section(0, 0, 0, 0, 0),
            section(1, 1, RODATA, rodata_at, len(rodata)),
            section(9, 2, 0, symtab_at, len(symbols), 3),
            section(17, 3, 0, strtab_at, len(strtab)),
            section(0, 3, 0, section_table_at + 5 * 40, len(section_names)),
        )
    )
    return header + rodata + symbols + strtab + sections + section_names


class BoardRamTests(unittest.TestCase):
    def run_checker(self, content, tier=None, pools=None, elf=NO_ELF):
        """`pools` is the pool table of the ELF next to the map; `elf` replaces it."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "image.map"
            path.write_text(content)
            if elf is NO_ELF:
                path.with_name("zephyr.elf").write_bytes(elf_image(pools))
            elif elf is not None:
                path.with_name("zephyr.elf").write_bytes(elf)
            command = [sys.executable, str(CHECKER), str(path)]
            if tier:
                command += ["--tier", tier]
            return subprocess.run(command, capture_output=True, text=True, check=False)

    def test_counts_output_sections_only_including_split_and_non_dot_names(self):
        result = self.run_checker(board_map())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            row("SRAM static demand", f"{BASE_DEMAND} / {SRAM_WINDOW}").rstrip(),
            result.stdout,
        )
        self.assertNotIn(".nested.input", result.stdout)
        self.assertIn("sw_isr_table", result.stdout)
        self.assertIn(".last_ram_section", result.stdout)
        self.assertIn(row("PSRAM static demand", "0 / 0").rstrip(), result.stdout)

    def test_nested_input_sections_are_never_counted(self):
        result = self.run_checker(
            board_map(
                extra_hp=".big            0x4ff20200      0x80\n"
                " .nested.input  0x4ff20200      0x40 object.o\n"
            )
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # the output section is counted once, the nested input never
        self.assertIn(
            row("SRAM static demand", f"{BASE_DEMAND + 0x80} / {SRAM_WINDOW}").rstrip(),
            result.stdout,
        )
        self.assertNotIn(".nested.input", result.stdout)

    def test_default_line_is_compact_and_its_boundary_is_exact(self):
        most = SRAM_WINDOW * 70 // 100 - BASE_DEMAND
        passed = self.run_checker(board_map(extra_hp=big(most)))
        failed = self.run_checker(board_map(extra_hp=big(most + 1)))
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertIn("PASS: total memory demand meets the compact line", passed.stdout)
        self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        self.assertIn(
            "FAIL: total memory demand exceeds the compact line", failed.stderr
        )
        self.assertIn("minimum", failed.stdout)

    def test_relaxed_line_boundary_is_exact(self):
        half = SRAM_WINDOW // 2 - BASE_DEMAND
        passed = self.run_checker(board_map(extra_hp=big(half)), tier="relaxed")
        failed = self.run_checker(board_map(extra_hp=big(half + 1)), tier="relaxed")
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        self.assertIn("exceeds the relaxed line", failed.stderr)

    def test_minimum_line_is_met_where_compact_is_not(self):
        content = board_map(extra_hp=big(600 * 1024))
        minimum = self.run_checker(content, tier="minimum")
        compact = self.run_checker(content, tier="compact")
        self.assertEqual(minimum.returncode, 0, minimum.stdout + minimum.stderr)
        self.assertNotEqual(compact.returncode, 0, compact.stdout + compact.stderr)
        self.assertIn(row("minimum", "PASS").rstrip(), minimum.stdout)
        self.assertIn(row("compact", "FAIL").rstrip(), compact.stdout)

    def test_minimum_line_boundary_is_exact(self):
        # SRAM filled to within `free_sram` bytes, plus a little more PSRAM
        # than the required free memory: what the PSRAM section leaves is
        # exactly the free memory.
        free_sram = SRAM_WINDOW - (BASE_DEMAND + 0x9FE00 + 0x20000)
        psram = 0x10000 + 200
        full = board_map(
            psram=psram,
            extra_hp=f"{big(0x9FE00)}\n{big(0x20000, 0x4FF00000)}",
        )
        self.assertEqual(free_sram, 96)
        last = free_sram + psram - 64 * 1024
        self.assertEqual(last, 296)
        passed = self.run_checker(
            full.replace(
                "OUTPUT(", f".ext_ram.data   0x48000000   0x{last:x}\nOUTPUT("
            ),
            tier="minimum",
        )
        failed = self.run_checker(
            full.replace(
                "OUTPUT(", f".ext_ram.data   0x48000000   0x{last + 1:x}\nOUTPUT("
            ),
            tier="minimum",
        )
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        self.assertIn("exceeds the minimum line", failed.stderr)

    def test_psram_counts_toward_both_memory_and_demand(self):
        result = self.run_checker(
            board_map(
                psram=0x2000000, psram_section=".ext_ram.data   0x48000000   0x300280"
            )
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for expected in (
            row(".ext_ram.data", 0x300280, "PSRAM"),
            row("PSRAM static demand", f"{0x300280} / {0x2000000}"),
            row(
                "Total memory demand",
                f"{0x300280 + BASE_DEMAND} / {0x2000000 + SRAM_WINDOW}",
            ),
        ):
            self.assertIn(expected.rstrip(), result.stdout)

    def test_psram_demand_decides_the_relaxed_line(self):
        within = board_map(
            psram=0x180000, psram_section=".ext_ram.data   0x48000000   0x100000"
        )
        beyond = board_map(
            psram=0x180000, psram_section=".ext_ram.data   0x48000000   0x120000"
        )
        self.assertEqual(self.run_checker(within, tier="relaxed").returncode, 0)
        compact = self.run_checker(beyond)
        self.assertEqual(compact.returncode, 0, compact.stdout + compact.stderr)
        relaxed = self.run_checker(beyond, tier="relaxed")
        self.assertNotEqual(relaxed.returncode, 0, relaxed.stdout + relaxed.stderr)
        self.assertIn(row("relaxed", "FAIL").rstrip(), relaxed.stdout)

    def test_empty_and_incomplete_maps_are_rejected(self):
        for content in (
            "",
            board_map().replace("OUTPUT(zephyr/zephyr.elf elf32-littleriscv)\n", ""),
            board_map().replace(
                "OUTPUT(zephyr/zephyr.elf elf32-littleriscv)", "OUTPUT("
            ),
            board_map().replace(".dram0.bss      0x4ff20150      0x40\n", ""),
            board_map().replace("sram0_0_seg", "different_segment"),
        ):
            with self.subTest(content=content):
                result = self.run_checker(content)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("FAIL", result.stderr)

    def test_overlapping_output_sections_are_rejected(self):
        result = self.run_checker(
            board_map(extra_hp=".overlap        0x4ff20100      0x80")
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL", result.stderr)
        result = self.run_checker(
            board_map(
                psram=0x100000,
                psram_section=(
                    ".ext_ram.data   0x48000000   0x100\n"
                    ".ext_ram.again  0x48000080   0x100"
                ),
            )
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FAIL", result.stderr)

    def test_section_straddling_a_memory_boundary_is_rejected(self):
        for content in (
            board_map(extra_hp=".straddle       0x4ffbfff0     0x100"),
            board_map(psram=0x1000, psram_section=".ext_ram.data   0x48000ff0   0x20"),
        ):
            with self.subTest(content=content):
                result = self.run_checker(content)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("crosses", result.stderr)

    def test_invalid_psram_region_is_rejected(self):
        for region in (
            # overlaps the HP SRAM window
            "ext_dram_seg     0x4ff00000         0x00001000         rw",
            # runs past the 32-bit address space
            "ext_dram_seg     0xfffffff0         0x00000020         rw",
        ):
            with self.subTest(region=region):
                content = board_map().replace(
                    "sram0_0_seg      0x4ff20000         0x0009cfc0         rw",
                    "sram0_0_seg      0x4ff20000         0x0009cfc0         rw\n"
                    + region,
                )
                result = self.run_checker(content)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("FAIL", result.stderr)

    def test_unknown_line_is_rejected(self):
        result = self.run_checker(board_map(), tier="generous")
        self.assertEqual(result.returncode, 2)

    def test_declared_pool_is_listed_and_checked_but_is_not_demand(self):
        pool = ("memory@48000000", 0x48000000, 0x2000000)
        result = self.run_checker(board_map(psram=0x2000000), pools=[pool])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for expected in (
            "memory@48000000 (declared pool)  33554432",
            row("Total memory demand", f"{BASE_DEMAND} / {0x2000000 + SRAM_WINDOW}"),
            row(
                "Static demand + declared pools",
                f"{BASE_DEMAND + 0x2000000} / {0x2000000 + SRAM_WINDOW}",
            ),
        ):
            self.assertIn(expected.rstrip(), result.stdout)

    def test_image_without_the_allocator_has_no_pool(self):
        result = self.run_checker(board_map(psram=0x2000000), pools=None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("(declared pool)", result.stdout)
        empty = self.run_checker(board_map(psram=0x2000000), pools=[])
        self.assertEqual(empty.returncode, 0, empty.stdout + empty.stderr)
        self.assertNotIn("(declared pool)", empty.stdout)

    def test_pool_may_also_be_carved_out_of_sram(self):
        result = self.run_checker(
            board_map(), pools=[("sram_pool", 0x4FF30000, 0x1000)]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            row(
                "Static demand + declared pools",
                f"{BASE_DEMAND + 0x1000} / {SRAM_WINDOW}",
            ).rstrip(),
            result.stdout,
        )

    def test_pool_overlapping_an_output_section_is_rejected(self):
        for content, pool, section in (
            (
                board_map(
                    psram=0x2000000, psram_section=".ext_ram.data   0x48001000   0x100"
                ),
                ("memory@48000000", 0x48000000, 0x2000000),
                ".ext_ram.data",
            ),
            (board_map(), ("sram_pool", 0x4FF20100, 0x40), "sw_isr_table"),
        ):
            with self.subTest(pool=pool):
                result = self.run_checker(content, pools=[pool])
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(
                    f"declared pool {pool[0]} overlaps output section {section}",
                    result.stderr,
                )

    def test_pool_outside_the_memory_or_crossing_a_boundary_is_rejected(self):
        for pool in (
            ("nowhere", 0x20000000, 0x1000),
            ("straddling", 0x47FF0000, 0x20000),
            ("too_large", 0x48000000, 0x2000001),
        ):
            with self.subTest(pool=pool):
                result = self.run_checker(board_map(psram=0x2000000), pools=[pool])
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("is not inside one memory window", result.stderr)

    def test_overlapping_pools_are_rejected(self):
        result = self.run_checker(
            board_map(psram=0x2000000),
            pools=[
                ("first", 0x48000000, 0x100000),
                ("second", 0x48080000, 0x100000),
            ],
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("declared pools overlap: first, second", result.stderr)

    def test_missing_or_unreadable_elf_is_rejected(self):
        for elf in (None, b"", b"not an elf at all", b"\x7fELF" + bytes(60)):
            with self.subTest(elf=elf):
                result = self.run_checker(board_map(), elf=elf)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("FAIL", result.stderr)


if __name__ == "__main__":
    unittest.main()
