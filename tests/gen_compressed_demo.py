#!/usr/bin/env python3
"""
生成一个用 RV32C 压缩指令写的 demo ELF。

程序只用 16 位指令完成：
    lui   a1, 0x10          # 消息地址高 20 位
    addi  a1, a1, msg_off   # 加上代码段内的偏移
    li    a0, 1             # fd = stdout
    li    a2, 31            # len
    li    a7, 8
    slli  a7, a7, 3         # a7 = 64 (write)
    ecall                   # 32-bit: 唯一的非压缩指令
    ebreak

用法:
    python tests/gen_compressed_demo.py build/rvc_demo.elf
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_demo_elf import BASE, write_elf  # noqa: E402

MSG = b"RVC works: 16-bit instructions\n"

REGS = {
    "zero": 0, "ra": 1, "sp": 2, "gp": 3, "tp": 4,
    "t0": 5, "t1": 6, "t2": 7, "s0": 8, "s1": 9,
    "a0": 10, "a1": 11, "a2": 12, "a3": 13, "a4": 14, "a5": 15,
    "a6": 16, "a7": 17,
    "s2": 18, "s3": 19, "s4": 20, "s5": 21, "s6": 22, "s7": 23,
    "s8": 24, "s9": 25, "s10": 26, "s11": 27,
    "t3": 28, "t4": 29, "t5": 30, "t6": 31,
}


def reg(x):
    return REGS[x] if isinstance(x, str) else int(x)


def sext6(v):
    v &= 0x3F
    return v | (~0x3F if v & 0x20 else 0)


# ---- RV32C 编码 (只覆盖 demo 用到的几条) ----
def c_li(rd, imm):
    imm &= 0x3F
    return (0b010 << 13) | (((imm >> 5) & 1) << 12) | \
           (reg(rd) << 7) | ((imm & 0x1F) << 2) | 0b01


def c_lui(rd, imm6):
    imm6 &= 0x3F
    assert imm6 != 0
    return (0b011 << 13) | (((imm6 >> 5) & 1) << 12) | \
           (reg(rd) << 7) | ((imm6 & 0x1F) << 2) | 0b01


def c_addi(rd, imm):
    imm &= 0x3F
    return (0b000 << 13) | (((imm >> 5) & 1) << 12) | \
           (reg(rd) << 7) | ((imm & 0x1F) << 2) | 0b01


def c_slli(rd, shamt):
    assert 0 <= shamt < 32
    return (0b000 << 13) | (((shamt >> 5) & 1) << 12) | \
           (reg(rd) << 7) | ((shamt & 0x1F) << 2) | 0b10


def c_ebreak():
    return 0x9002


def build():
    code = bytearray()

    # 先把固定长度的指令编出来，算消息偏移
    # lui a1, 0x10 ; addi a1, msg_off ; li a0,1 ; li a2,len ;
    # li a7,8 ; slli a7,7,3 ; ecall ; ebreak
    fixed_len = 6 * 2 + 4 + 2
    msg_off = fixed_len
    assert 0 < msg_off < 32, "c.addi 的 6 位有符号立即数放不下"

    insns16 = [
        c_lui("a1", 0x10),
        c_addi("a1", msg_off),
        c_li("a0", 1),
        c_li("a2", len(MSG)),
        c_li("a7", 8),
        c_slli("a7", 3),
    ]
    for insn in insns16:
        code += struct.pack("<H", insn)

    code += struct.pack("<I", 0x00000073)   # ecall
    code += struct.pack("<H", c_ebreak())   # ebreak
    code += MSG

    write_elf(sys.argv[1] if len(sys.argv) > 1 else "build/rvc_demo.elf",
              bytes(code), BASE)
    print("[gen] message at 0x%08X (offset %d)" % (BASE + msg_off, msg_off))


if __name__ == "__main__":
    build()
