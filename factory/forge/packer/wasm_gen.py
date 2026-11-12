#!/usr/bin/env python3
"""Forge 最小 wasm 组装器（D-3）：任务书三导出模块的确定性字节组装。

背景如实：SDK 三轮产物（run001 84B/run002 94B）均被设备 WAMR 验证
拒（分支栈型/hang 尾值）。R12 合同=Forge 掌握产物约束——本组装器
为工厂线的确定性后端（LLM 产码→确定性组装→签名；wabt 不入依赖）。

模块（与 tasks/factory-brief.md 规格一致）：
  run(N)   = 2N+1
  spin(N)  = N 次空转后返回 0（block/loop：N<=0 跳出，否则递减回边；
             RF1 #10：旧形 br_if 在递减之前回边，spin(N>=1) 死循环）
  hang(N)  = 无条件死循环（loop br 0；尾 const 0 满足静态 result）
"""

import sys
from pathlib import Path


def leb_u(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def section(sid: int, body: bytes) -> bytes:
    return bytes([sid]) + leb_u(len(body)) + body


def name_bytes(s: str) -> bytes:
    b = s.encode()
    return leb_u(len(b)) + b


def build() -> bytes:
    # type section：1 个 (i32)->(i32)
    types = leb_u(1) + bytes([0x60, 0x01, 0x7F, 0x01, 0x7F])
    # function section：3 函数全 type0
    funcs = leb_u(3) + bytes([0x00, 0x00, 0x00])
    # export section：run/spin/hang -> func 0/1/2
    exps = (
        leb_u(3)
        + name_bytes("run")
        + bytes([0x00, 0x00])
        + name_bytes("spin")
        + bytes([0x00, 0x01])
        + name_bytes("hang")
        + bytes([0x00, 0x02])
    )
    # code bodies（locals=00 前缀）
    run = bytes(
        [
            0x00,  # locals 0
            0x20,
            0x00,  # local.get 0
            0x41,
            0x02,  # i32.const 2
            0x6C,  # i32.mul
            0x41,
            0x01,  # i32.const 1
            0x6A,  # i32.add
            0x0B,
        ]
    )  # end
    spin = bytes(
        [
            0x00,  # locals 0
            0x02,
            0x40,  # block $b
            0x03,
            0x40,  #   loop $l
            0x20,
            0x00,  #     local.get 0
            0x41,
            0x00,  #     i32.const 0
            0x4C,  #     i32.le_s
            0x0D,
            0x01,  #     br_if $b（N<=0 跳出）
            0x20,
            0x00,  #     local.get 0
            0x41,
            0x01,  #     i32.const 1
            0x6B,  #     i32.sub
            0x21,
            0x00,  #     local.set 0
            0x0C,
            0x00,  #     br $l（递减后回边）
            0x0B,  #   end loop
            0x0B,  # end block
            0x41,
            0x00,  # i32.const 0（result）
            0x0B,
        ]
    )  # end func
    hang = bytes(
        [
            0x00,  # locals 0
            0x03,
            0x40,  # loop
            0x0C,
            0x00,  #   br $l（无条件回边）
            0x0B,  # end loop
            0x41,
            0x00,  # i32.const 0（静态 result——
            #  验证器要求不可达也满足）
            0x0B,
        ]
    )  # end func
    code = (
        leb_u(3)
        + leb_u(len(run))
        + run
        + leb_u(len(spin))
        + spin
        + leb_u(len(hang))
        + hang
    )
    mod = (
        b"\x00asm\x01\x00\x00\x00"
        + section(1, types)
        + section(3, funcs)
        + section(7, exps)
        + section(10, code)
    )
    return mod


if __name__ == "__main__":
    out = Path(sys.argv[1])
    mod = build()
    out.write_bytes(mod)
    print(f"OK: {out} {len(mod)}B")
