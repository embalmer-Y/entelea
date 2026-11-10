#!/usr/bin/env python3
"""D12 静态 SRAM 红线断言（owner v1：≤540KB=70%）。

读 zephyr.map 的 linker Memory Configuration 与段表，对地址落在
HP SRAM 窗 [0x4ff00000, 0x4ffc0000) 的已分配段求和（noinit 计入
物理占用——board 表 §2 口径）并断言。用法：
    python3 tools/check_board_ram.py build/<app>/zephyr/zephyr.map [--limit 552960]
"""
import argparse
import re
import sys

HP_LO = 0x4FF00000
HP_HI = 0x4FFC0000


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mapfile")
    ap.add_argument("--limit", type=int, default=540 * 1024)
    args = ap.parse_args()
    total = 0
    matched = []
    for line in open(args.mapfile, errors="ignore"):
        m = re.match(r"^\s*(\.[\w.]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", line)
        if not m:
            continue
        name, addr, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if size == 0 or addr < HP_LO or addr >= HP_HI:
            continue
        if name.startswith(".flash") or name.startswith(".rtc"):
            continue
        total += size
        matched.append((name, size))
    for name, size in matched:
        print(f"{name:32s} {size:>8}")
    print(f"{'TOTAL':32s} {total:>8} / limit {args.limit}")
    if total > args.limit:
        print("FAIL: static HP SRAM exceeds D12 red line (70%)", file=sys.stderr)
        return 1
    print("PASS: static HP SRAM within D12 red line")
    return 0


if __name__ == "__main__":
    sys.exit(main())
