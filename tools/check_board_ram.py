#!/usr/bin/env python3
"""D12 内存红线断言：三档，按总内存（owner 2026-11-24）。

读 GNU ld zephyr.map 的目标内存配置与输出段表，以及镜像 ELF 里设备树声明的
分配池。总内存 M 是 HP SRAM 窗 [0x4ff00000, 0x4ffc0000) 加上映射文件声明的
PSRAM（ext_dram_seg，没有则为 0）。总内存需求 D 是落在这两处的输出段之和
（含 noinit、代码及内核段；不重复计算嵌套输入段）。三档：

    最低  M - D >= 65536    （运行时堆余量 >= 64 KB）
    紧凑  D <= 0.70 M
    宽松  D <= 0.50 M

镜像必须满足 --tier 指定的档，默认紧凑（等于本标准之前在本板上的单一
红线）；三档的结果都打印。缺少必要地标、段越界或重叠时失败；仅凭文本地标
不能证明任意 map 未被删改，应与相应 ELF 对拍。

分配池（platform/zephyr/libc_heap.c）是板级设备树里带 zephyr,memory-attr 的
内存区域，由 libc 的分配器在运行期使用。它不在映射文件里，从 ELF 的
mem_attr_region 表读出（--elf，默认取映射文件旁的 zephyr.elf；没有可读的 ELF
即失败）。池子是运行期堆，与此前“剩余 SRAM 即 libc 堆”的口径一致，不计入 D，
单独列出并折算成“静态占用加池子”；但它必须整个落在内存范围内，且不与任何
输出段或其它池子重叠，否则失败。用法：
    python3 tools/check_board_ram.py build/<app>/zephyr/zephyr.map [--tier compact] [--elf zephyr.elf]
"""

import argparse
import re
import struct
import sys
from pathlib import Path

HP_LO = 0x4FF00000
HP_HI = 0x4FFC0000
PSRAM_ADDRESS_LIMIT = 0x1_0000_0000
MINIMUM_FREE = 64 * 1024
REGION = re.compile(r"^sram0_0_seg\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\b")
PSRAM_REGION = re.compile(r"^ext_dram_seg\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\b")
SECTION = re.compile(
    r"^([A-Za-z_.$][\w.$-]*)(?:[ \t]+(0x[0-9a-fA-F]+)"
    r"[ \t]+(0x[0-9a-fA-F]+)(?:[ \t]+.*)?)?[ \t]*$"
)
CONTINUATION = re.compile(r"^[ \t]+(0x[0-9a-fA-F]+)[ \t]+(0x[0-9a-fA-F]+)(?:[ \t]|$)")
OUTPUT = re.compile(r"^OUTPUT\(\S+\.elf[ \t]+elf32-littleriscv\)[ \t]*$")
REQUIRED = {".iram0.text", ".dram0.data", ".dram0.noinit", ".dram0.bss"}
# ELF32: section header, symbol, and struct mem_attr_region_t on a 32-bit target
# (name pointer, address, size, attributes).
ELF_SECTION = struct.Struct("<10I")
ELF_SYMBOL = struct.Struct("<IIIBBH")
POOL_ENTRY = struct.Struct("<IIII")
SHT_SYMTAB = 2
SHT_NOBITS = 8
POOL_TABLE_SYMBOL = "mem_attr_region"


def output_sections(mapfile: str) -> tuple[list, list, int, int]:
    """Return the output sections in HP SRAM and in PSRAM, and the PSRAM base and size."""
    region_ok = False
    in_map = False
    complete = False
    pending = None
    hp_sections = []
    psram_sections = []
    psram_lo = 0
    psram_size = 0
    seen = set()

    def add(name: str, address: str, length: str) -> None:
        addr, size = int(address, 16), int(length, 16)
        end = addr + size
        windows = [(HP_LO, HP_HI, hp_sections, "HP SRAM")]
        if psram_size:
            psram_hi = psram_lo + psram_size
            windows.append((psram_lo, psram_hi, psram_sections, "PSRAM"))
        for lo, hi, bucket, label in windows:
            if size and addr < hi and end > lo:
                if addr < lo or end > hi:
                    raise ValueError(f"section {name} crosses {label} boundary")
                bucket.append((name, addr, size))
        if HP_LO <= addr < HP_HI:
            seen.add(name)

    with open(mapfile, encoding="utf-8", errors="strict") as source:
        for line in source:
            if not in_map:
                region = REGION.match(line)
                if region:
                    origin, length = int(region[1], 16), int(region[2], 16)
                    region_ok = HP_LO <= origin < HP_HI and 0 < length <= HP_HI - origin
                psram = PSRAM_REGION.match(line)
                if psram:
                    psram_lo, psram_size = int(psram[1], 16), int(psram[2], 16)
                    psram_hi = psram_lo + psram_size
                    if (
                        psram_size <= 0
                        or psram_hi > PSRAM_ADDRESS_LIMIT
                        or (psram_lo < HP_HI and psram_hi > HP_LO)
                    ):
                        raise ValueError("invalid PSRAM region ext_dram_seg")
                if line.strip() == "Linker script and memory map":
                    in_map = True
                continue
            if line.startswith("OUTPUT("):
                complete = bool(OUTPUT.match(line.rstrip("\n")))
                break
            if pending is not None:
                continuation = CONTINUATION.match(line)
                if continuation:
                    add(pending, continuation[1], continuation[2])
                    pending = None
                    continue
                pending = None
            section = SECTION.match(line.rstrip("\n"))
            if section:
                name, address, length = section.groups()
                if address is None:
                    pending = name
                else:
                    add(name, address, length)

    if not in_map or not region_ok or not complete:
        raise ValueError("missing target memory region or complete linker map")
    missing = REQUIRED - seen
    if missing:
        raise ValueError(f"missing HP output sections: {', '.join(sorted(missing))}")
    for bucket in (hp_sections, psram_sections):
        ordered = sorted(bucket, key=lambda section: section[1])
        for previous, current in zip(ordered, ordered[1:]):
            if previous[1] + previous[2] > current[1]:
                raise ValueError(
                    f"overlapping output sections: {previous[0]}, {current[0]}"
                )
    return hp_sections, psram_sections, psram_lo, psram_size


def declared_pools(elffile) -> list:
    """Return [(name, address, size)]: the allocation pools linked into the image.

    Zephyr builds the table from the devicetree memory nodes that carry
    zephyr,memory-attr; an image that does not use the allocator has no table.
    """
    data = Path(elffile).read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4:6] != b"\x01\x01":
        raise ValueError("declared pools need a 32-bit little-endian ELF")
    section_offset = struct.unpack_from("<I", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 46)
    if entry_size < ELF_SECTION.size or section_offset + count * entry_size > len(data):
        raise ValueError("malformed ELF section table")
    sections = [
        ELF_SECTION.unpack_from(data, section_offset + i * entry_size)
        for i in range(count)
    ]

    def string_at(base: int, offset: int) -> bytes:
        return data[base + offset :].split(b"\x00", 1)[0]

    def bytes_at(address: int, size: int) -> bytes:
        for section in sections:
            kind, start, offset, length = section[1], section[3], section[4], section[5]
            if (
                kind != SHT_NOBITS
                and start <= address
                and address + size <= start + length
            ):
                return data[offset + address - start : offset + address - start + size]
        raise ValueError("declared pool table is not in a loaded section")

    def cstring_at(address: int) -> str:
        """The NUL-terminated string at `address`, bounded by its section."""
        for section in sections:
            kind, start, offset, length = section[1], section[3], section[4], section[5]
            if kind != SHT_NOBITS and start <= address < start + length:
                begin = offset + address - start
                return (
                    data[begin : offset + length]
                    .split(b"\x00", 1)[0]
                    .decode("utf-8", "replace")
                )
        raise ValueError("a declared pool name is not in a loaded section")

    symbol_table = next((s for s in sections if s[1] == SHT_SYMTAB), None)
    if symbol_table is None:
        raise ValueError("the ELF has no symbol table")
    strings = sections[symbol_table[6]]
    table = None
    for offset in range(
        symbol_table[4], symbol_table[4] + symbol_table[5], ELF_SYMBOL.size
    ):
        name, value, size, _info, _other, _index = ELF_SYMBOL.unpack_from(data, offset)
        if string_at(strings[4], name) == POOL_TABLE_SYMBOL.encode():
            table = (value, size)
            break
    if table is None:
        return []
    value, size = table
    if size % POOL_ENTRY.size:
        raise ValueError("malformed declared pool table")
    pools = []
    for index in range(size // POOL_ENTRY.size):
        name, address, length, _attr = POOL_ENTRY.unpack(
            bytes_at(value + index * POOL_ENTRY.size, POOL_ENTRY.size)
        )
        pools.append((cstring_at(name), address, length))
    return pools


def check_pools(pools: list, windows: list) -> None:
    """Fail unless every pool lies inside one memory window and overlaps nothing.

    `windows` holds (low, high, sections, label) per memory; pools may neither
    cross a window boundary nor overlap an output section or another pool.
    """
    for index, (name, address, size) in enumerate(pools):
        end = address + size
        home = next((w for w in windows if address >= w[0] and end <= w[1]), None)
        if home is None:
            raise ValueError(f"declared pool {name} is not inside one memory window")
        for section, start, length in home[2]:
            if length and start < end and address < start + length:
                raise ValueError(
                    f"declared pool {name} overlaps output section {section}"
                )
        for other, other_address, other_size in pools[index + 1 :]:
            if other_address < end and address < other_address + other_size:
                raise ValueError(f"declared pools overlap: {name}, {other}")


def tier_results(demand: int, total: int) -> list:
    """Return (tier, passed, description) for the three lines, lowest first."""
    return [
        ("minimum", total - demand >= MINIMUM_FREE, f"free >= {MINIMUM_FREE}"),
        ("compact", demand * 100 <= total * 70, "demand <= 70%"),
        ("relaxed", demand * 2 <= total, "demand <= 50%"),
    ]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mapfile")
    ap.add_argument(
        "--tier",
        choices=("minimum", "compact", "relaxed"),
        default="compact",
        help="line the image must meet (default: compact)",
    )
    ap.add_argument(
        "--elf",
        help="image ELF to read the declared pools from (default: zephyr.elf next to the map)",
    )
    args = ap.parse_args()
    elf = args.elf or str(Path(args.mapfile).with_name("zephyr.elf"))
    try:
        hp_sections, psram_sections, psram_base, psram_size = output_sections(
            args.mapfile
        )
        windows = [(HP_LO, HP_HI, hp_sections, "SRAM")]
        if psram_size:
            windows.append(
                (psram_base, psram_base + psram_size, psram_sections, "PSRAM")
            )
        pools = declared_pools(elf)
        check_pools(pools, windows)
    except (OSError, UnicodeError, ValueError, struct.error) as exc:
        print(f"FAIL: invalid board RAM map: {exc}", file=sys.stderr)
        return 1
    for label, sections in (("SRAM", hp_sections), ("PSRAM", psram_sections)):
        for name, _, size in sections:
            print(f"{name:32s} {size:>8}  {label}")
    pool_total = 0
    for name, _, size in pools:
        pool_total += size
        print(f"{name + ' (declared pool)':32s} {size:>8}")
    hp_total = sum(size for _, _, size in hp_sections)
    psram_total = sum(size for _, _, size in psram_sections)
    demand = hp_total + psram_total
    total = HP_HI - HP_LO + psram_size
    print(f"{'SRAM static demand':32s} {hp_total:>8} / {HP_HI - HP_LO}")
    print(f"{'PSRAM static demand':32s} {psram_total:>8} / {psram_size}")
    print(f"{'Total memory demand':32s} {demand:>8} / {total}")
    print(f"{'Static demand + declared pools':32s} {demand + pool_total:>8} / {total}")
    passed = {}
    for tier, ok, rule in tier_results(demand, total):
        passed[tier] = ok
        print(f"{tier:32s} {'PASS' if ok else 'FAIL':>8}  ({rule})")
    if not passed[args.tier]:
        print(
            f"FAIL: total memory demand exceeds the {args.tier} line", file=sys.stderr
        )
        return 1
    print(f"PASS: total memory demand meets the {args.tier} line")
    return 0


if __name__ == "__main__":
    sys.exit(main())
