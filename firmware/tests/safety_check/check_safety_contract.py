#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""L5 安全合同机械检查（docs/std/testing.md §3；CI 独立 job，一票否决）。

六项检查：
  1. 唯一写路径：Zephyr 输出驱动调用只允许出现在 ts-safety driver_dispatch.c。
  2. 三安全态注册完备：ts_out_ch_t 初始化块必须含 .poweron/.linkloss/.fault。
  3. 禁用模式：框架源码（module+app）禁未播种随机/墙钟；k_uptime 只允许在 time.c。
  4. 常量出处：可调常量（限值/超时/尺寸类 #define TS_*）须有 DEC/Q/DR/推导 类出处标注。
  5. estop 路径：ts_safety_force_all_fault 函数体与 driver_dispatch.c 内禁
     分配/队列/锁/协议栈/睡眠符号。
  6. prov 零写：prov.c 不含任何后端写调用（合同 10；LLD-ts-store §8 扩展位，
     写通道在 prov_test.c/烧录工具）。

已知边界（登记于 M1 交付报告）：检查 2 对宏展开的通道描述符不生效（如测试的
REPLAY_CH 宏）——宏体内字段由检查 4 的出处纪律与 code review 兜底。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FW = REPO / "firmware"
MODULE_SRC = FW / "module" / "tessera" / "src"
DISPATCH_FILE = MODULE_SRC / "safety" / "driver_dispatch.c"
TIME_FILE = MODULE_SRC / "core" / "time.c"

OUTPUT_DRIVER_RE = re.compile(
    r"\b(gpio_pin_set(?:_raw|_dt)?|gpio_port_set_bits_raw|gpio_port_clear_bits_raw"
    r"|pwm_set(?:_(?:cycles|duty|pulse|ticks))?(?:_dt)?)\s*\("
)
# 注（IR2-04/DEC-48 批）：覆盖裸 pwm_set(——本库 PWM 落驱动即此形态
#（driver_dispatch.c），原正则只匹配带后缀变体 = 机械防线缺口。
CH_INIT_RE = re.compile(r"\bts_out_ch_t\s+\w+\s*=\s*\{")
FORBIDDEN_TIME_RE = re.compile(
    r"\b(srand|rand|random)\s*\(|\btime\s*\(|\blocaltime\s*\(|\bgmtime\s*\("
    r"|\bclock_gettime\s*\(|\bk_uptime_\w+\s*\("
)
# 检查 4 豁免族：枚举/结构性 define（非可调常量）
ENUM_DEFINE_RE = re.compile(r"^#define\s+(TS_(?:OK\b|E_|FAIL_SRC_)[A-Z0-9_]*)\b")
TUNABLE_DEFINE_RE = re.compile(r"^#define\s+(TS_[A-Z0-9_]+)\s+[-(]?\d")
PROVENANCE_RE = re.compile(r"(DEC-\d|Q-\d|DR-\d|SC-\d|LLD|HLD|结构性|推导|M0 skeleton|Source:)")
ESTOP_FORBIDDEN_RE = re.compile(
    r"\b(k_(?:malloc|calloc|free|mutex\w*|sem\w*|msgq\w*|fifo\w*|queue\w*|work\w*|sleep))\b"
    r"|\b(net_\w+|zenoh\w*|printk)\b"
)


def c_files(roots: list[Path]) -> list[Path]:
    out: list[Path] = []
    for root in roots:
        out.extend(p for p in root.rglob("*.c") if p.is_file())
    return out


# 唯一写路径豁免：driver_dispatch（驱动分发点）+ boardbench 基准（裸 GPIO
# 对照层——无它无法量化框架增量，DEC-44；产品代码直调仍一票否决）。
WRITE_PATH_ALLOWED = {DISPATCH_FILE, FW / "tests" / "boardbench" / "src" / "main.c"}


def check_1_write_path(files: list[Path]) -> list[str]:
    bad = []
    for f in files:
        for m in OUTPUT_DRIVER_RE.finditer(f.read_text(encoding="utf-8")):
            if f not in WRITE_PATH_ALLOWED:
                bad.append(f"[1] 输出驱动调用越权: {f.relative_to(REPO)}:{m.group(1)}")
    return bad


def _init_blocks(text: str):
    for m in CH_INIT_RE.finditer(text):
        depth, i = 1, m.end() - 1
        while i < len(text) and depth > 0:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield m.start(), text[m.start():i]


def check_2_three_states(files: list[Path]) -> list[str]:
    bad = []
    for f in files:
        text = f.read_text(encoding="utf-8")
        for pos, block in _init_blocks(text):
            for field in (".poweron", ".linkloss", ".fault"):
                if field not in block:
                    line = text.count("\n", 0, pos) + 1
                    bad.append(f"[2] 通道描述符缺 {field}: {f.relative_to(REPO)}:{line}")
    return bad


def check_3_forbidden_patterns() -> list[str]:
    bad = []
    for f in c_files([FW / "module", FW / "app"]):
        text = f.read_text(encoding="utf-8")
        for m in FORBIDDEN_TIME_RE.finditer(text):
            # k_uptime 白名单：唯一时间源实现（LLD-ts-core §3）
            if f == TIME_FILE and m.group(0).startswith("k_uptime"):
                continue
            line = text.count("\n", 0, m.start()) + 1
            bad.append(f"[3] 禁用模式（随机/墙钟/uptime）: {f.relative_to(REPO)}:{line}:{m.group(0).strip()}")
    return bad


def check_4_constant_provenance() -> list[str]:
    bad = []
    for f in list((FW / "module").rglob("*.[ch]")) + list((FW / "app").rglob("*.[ch]")):
        lines = f.read_text(encoding="utf-8").splitlines()
        for idx, line in enumerate(lines):
            if ENUM_DEFINE_RE.match(line):
                continue
            if not TUNABLE_DEFINE_RE.match(line):
                continue
            ctx = "\n".join(lines[max(0, idx - 3): idx + 1])
            if not PROVENANCE_RE.search(ctx):
                bad.append(
                    f"[4] 可调常量缺出处（DEC/Q/DR/推导）: {f.relative_to(REPO)}:{idx + 1}:{line.strip()}"
                )
    return bad


def _func_body(text: str, name: str) -> str | None:
    m = re.search(rf"\b{name}\s*\(", text)
    if m is None:
        return None
    depth, i = 0, m.end()
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start():i]
        i += 1
    return text[m.start():]


def check_5_estop_path() -> list[str]:
    bad = []
    force_c = (MODULE_SRC / "safety" / "force.c").read_text(encoding="utf-8")
    body = _func_body(force_c, "ts_safety_force_all_fault")
    if body is None:
        bad.append("[5] 未找到 ts_safety_force_all_fault（检查失效）")
    else:
        for m in ESTOP_FORBIDDEN_RE.finditer(body):
            bad.append(f"[5] estop 调用图禁用符号: force.c:{m.group(0)}")
    dispatch = DISPATCH_FILE.read_text(encoding="utf-8")
    for m in ESTOP_FORBIDDEN_RE.finditer(dispatch):
        bad.append(f"[5] driver_dispatch 禁用符号: {m.group(0)}")
    return bad


def check_6_prov_no_write() -> list[str]:
    """LLD-ts-store §8 扩展位：prov.c 零写调用（合同 10——写通道仅烧录期）。"""
    prov_c = MODULE_SRC / "store" / "prov.c"
    if not prov_c.exists():
        return []
    write_re = re.compile(r"\bts_store_backend\s*\.\s*write|\bts_store_prov_write|\.write\s*\(")
    bad = []
    for i, line in enumerate(prov_c.read_text(encoding="utf-8").splitlines(), 1):
        if write_re.search(line) and not line.strip().startswith("*") and not line.strip().startswith("/*"):
            bad.append(f"[6] prov.c 含写调用: prov.c:{i}:{line.strip()}")
    return bad


def check_7_slot_choke_point() -> list[str]:
    """V0.8.0 EC4（Q-40②/DEC-60 · LLD-ts-appmgr §5b 侧载纪律）：TSAP 装入
    slot 的唯一写路径 = appmgr stage 链（pkg.c）。任何安装通道（zenoh 部署 /
    回滚翻转 / 未来 SD/USB 侧载——OpenWrt 反面教训，R9 §6）必经
    stage_begin/chunk → verify_slot 同一验签收口，无第二写路径。机械保证：
    模块生产代码中 ts_store_slot_write/erase 调用点仅允许出现在
    src/appmgr/pkg.c（定义处 store/slot.c 豁免；测试面不在扫描域）。"""
    bad = []
    for f in MODULE_SRC.rglob("*.c"):
        rel = str(f.relative_to(REPO))
        if rel.endswith(("store/slot.c", "appmgr/pkg.c")):
            continue  # 定义域（公共 API 提供方）/ 唯一合法消费点（stage 链）
        for i, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
            s = line.strip()
            if s.startswith("*") or s.startswith("/*"):
                continue
            if re.search(r"\bts_store_slot_(write|erase)\s*\(", line):
                bad.append(f"[7] slot 写路径越界（唯一收口 = pkg.c stage 链）: "
                           f"{rel}:{i}:{s}")
    return bad


def main() -> int:
    violations: list[str] = []
    all_c = c_files([FW / "module", FW / "app", FW / "tests"])
    violations += check_1_write_path(all_c)
    violations += check_2_three_states(all_c)
    violations += check_3_forbidden_patterns()
    violations += check_4_constant_provenance()
    violations += check_5_estop_path()
    violations += check_6_prov_no_write()
    violations += check_7_slot_choke_point()

    if violations:
        print("L5 FAILED（安全合同机械检查，testing.md §3）:")
        for v in violations:
            print("  " + v)
        return 1
    print("L5 OK: 7/7 机械检查通过（唯一写路径/三态完备/禁用模式/常量出处/estop 路径/prov 零写/slot 收口）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
