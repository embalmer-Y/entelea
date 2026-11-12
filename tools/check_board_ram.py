#!/usr/bin/env python3
"""D12 静态 SRAM 红线断言（owner v1：≤540KB=70%）。

读 GNU ld zephyr.map 的目标内存配置与输出段表，对 HP SRAM 窗
[0x4ff00000, 0x4ffc0000) 内的输出段求和（含 noinit、代码及内核
段；不重复计算嵌套输入段）。缺少必要地标、段越界或重叠时失败；仅凭
文本地标不能证明任意 map 未被删改，应与相应 ELF 对拍。用法：
    python3 tools/check_board_ram.py build/<app>/zephyr/zephyr.map [--limit 552960]
"""

import argparse
import re
import sys

HP_LO = 0x4FF00000
HP_HI = 0x4FFC0000
REGION = re.compile(r"^sram0_0_seg\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\b")
SECTION = re.compile(
    r"^([A-Za-z_.$][\w.$-]*)(?:[ \t]+(0x[0-9a-fA-F]+)"
    r"[ \t]+(0x[0-9a-fA-F]+)(?:[ \t]+.*)?)?[ \t]*$"
)
CONTINUATION = re.compile(r"^[ \t]+(0x[0-9a-fA-F]+)[ \t]+(0x[0-9a-fA-F]+)(?:[ \t]|$)")
OUTPUT = re.compile(r"^OUTPUT\(\S+\.elf[ \t]+elf32-littleriscv\)[ \t]*$")
REQUIRED = {".iram0.text", ".dram0.data", ".dram0.noinit", ".dram0.bss"}


def output_sections(mapfile: str) -> list[tuple[str, int, int]]:
    region_ok = False
    in_map = False
    complete = False
    pending = None
    sections = []
    seen = set()

    def add(name: str, address: str, length: str) -> None:
        addr, size = int(address, 16), int(length, 16)
        end = addr + size
        if size and addr < HP_HI and end > HP_LO:
            if addr < HP_LO or end > HP_HI:
                raise ValueError(f"section {name} crosses HP SRAM boundary")
            sections.append((name, addr, size))
        if HP_LO <= addr < HP_HI:
            seen.add(name)

    with open(mapfile, encoding="utf-8", errors="strict") as source:
        for line in source:
            if not in_map:
                region = REGION.match(line)
                if region:
                    origin, length = int(region[1], 16), int(region[2], 16)
                    region_ok = HP_LO <= origin < HP_HI and 0 < length <= HP_HI - origin
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
    ordered = sorted(sections, key=lambda section: section[1])
    for previous, current in zip(ordered, ordered[1:]):
        if previous[1] + previous[2] > current[1]:
            raise ValueError(
                f"overlapping output sections: {previous[0]}, {current[0]}"
            )
    return sections


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mapfile")
    ap.add_argument("--limit", type=int, default=540 * 1024)
    args = ap.parse_args()
    if args.limit < 0:
        ap.error("--limit must be non-negative")
    try:
        sections = output_sections(args.mapfile)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"FAIL: invalid board RAM map: {exc}", file=sys.stderr)
        return 1
    total = sum(size for _, _, size in sections)
    for name, _, size in sections:
        print(f"{name:32s} {size:>8}")
    print(f"{'TOTAL':32s} {total:>8} / limit {args.limit}")
    if total > args.limit:
        print("FAIL: static HP SRAM exceeds D12 red line (70%)", file=sys.stderr)
        return 1
    print("PASS: static HP SRAM within D12 red line")
    return 0


if __name__ == "__main__":
    sys.exit(main())
