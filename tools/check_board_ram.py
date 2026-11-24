#!/usr/bin/env python3
"""D12 内存红线断言：三档，按总内存（owner 2026-11-24）。

读 GNU ld zephyr.map 的目标内存配置与输出段表。总内存 M 是 HP SRAM 窗
[0x4ff00000, 0x4ffc0000) 加上映射文件声明的 PSRAM（ext_dram_seg，没有则
为 0）。总内存需求 D 是落在这两处的输出段之和（含 noinit、代码及内核
段；不重复计算嵌套输入段）。三档：

    最低  M - D >= 65536    （运行时堆余量 >= 64 KB）
    紧凑  D <= 0.70 M
    宽松  D <= 0.50 M

镜像必须满足 --tier 指定的档，默认紧凑（等于本标准之前在本板上的单一
红线）；三档的结果都打印。缺少必要地标、段越界或重叠时失败；仅凭文本地标
不能证明任意 map 未被删改，应与相应 ELF 对拍。用法：
    python3 tools/check_board_ram.py build/<app>/zephyr/zephyr.map [--tier compact]
"""

import argparse
import re
import sys

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


def output_sections(mapfile: str) -> tuple[list, list, int]:
    """Return the output sections in HP SRAM, those in PSRAM, and the PSRAM size."""
    region_ok = False
    in_map = False
    complete = False
    pending = None
    hp_sections = []
    psram_sections = []
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
    return hp_sections, psram_sections, psram_size


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
    args = ap.parse_args()
    try:
        hp_sections, psram_sections, psram_size = output_sections(args.mapfile)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"FAIL: invalid board RAM map: {exc}", file=sys.stderr)
        return 1
    for label, sections in (("SRAM", hp_sections), ("PSRAM", psram_sections)):
        for name, _, size in sections:
            print(f"{name:32s} {size:>8}  {label}")
    hp_total = sum(size for _, _, size in hp_sections)
    psram_total = sum(size for _, _, size in psram_sections)
    demand = hp_total + psram_total
    total = HP_HI - HP_LO + psram_size
    print(f"{'SRAM static demand':32s} {hp_total:>8} / {HP_HI - HP_LO}")
    print(f"{'PSRAM static demand':32s} {psram_total:>8} / {psram_size}")
    print(f"{'Total memory demand':32s} {demand:>8} / {total}")
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
