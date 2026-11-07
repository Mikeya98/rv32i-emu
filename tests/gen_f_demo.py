#!/usr/bin/env python3
"""
生成一个演示 RV32F 的 RV32IMAF ELF。

程序流程:
    1. 把 1.5f / 2.25f / 9.0f 写进内存, 用 flw 读进 f 寄存器
    2. fadd 1.5 + 2.25 = 3.75
    3. fmul 1.5 * 2.25 = 3.375
    4. fsqrt 9.0 = 3.0
    5. fcvt.w.s (RTZ) 3.0 = 3
    6. fsw/fsw 往返: fadd 的结果写回内存再用 lw 读出来
    每项成功打印一个 'F', 失败打印 'X'

预期输出:
    FFFFF

用法:
    python tests/gen_f_demo.py build/f_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

UART = 0x20000000
DATA_ADDR = 0x11000


def fp(f7, rs2, rs1, f3, rd):
    return ((f7 & 0x7F) << 25) | ((rs2 & 0x1F) << 20) | \
           ((rs1 & 0x1F) << 15) | ((f3 & 7) << 12) | \
           ((rd & 0x1F) << 7) | 0x53


def flw(imm, rs1, fd):
    return ((imm & 0xFFF) << 20) | ((rs1 & 0x1F) << 15) | \
           (2 << 12) | ((fd & 0x1F) << 7) | 0x07


def fsw(imm, fs2, rs1):
    return (((imm >> 5) & 0x7F) << 25) | ((fs2 & 0x1F) << 20) | \
           ((rs1 & 0x1F) << 15) | (2 << 12) | \
           ((imm & 0x1F) << 7) | 0x27


def build():
    asm = Assembler()
    asm.label("_start")

    # s2 = UART, s3 = 数据区
    asm.emit("li32", "s2", UART)
    asm.emit("li32", "s3", DATA_ADDR)

    # mem[0] = 1.5f, mem[4] = 2.25f, mem[8] = 9.0f
    asm.emit("li32", "t0", 0x3FC00000)
    asm.emit("sw", "t0", "s3", 0)
    asm.emit("li32", "t0", 0x40100000)
    asm.emit("sw", "t0", "s3", 4)
    asm.emit("li32", "t0", 0x41100000)
    asm.emit("sw", "t0", "s3", 8)

    asm.emit("word", flw(0, 19, 1))      # flw f1, 0(s3)
    asm.emit("word", flw(4, 19, 2))      # flw f2, 4(s3)
    asm.emit("word", flw(8, 19, 6))      # flw f6, 8(s3)

    # ---- fadd 1.5 + 2.25 = 3.75 ----
    asm.emit("word", fp(0x00, 2, 1, 0, 3))
    asm.emit("word", fp(0x70, 0, 3, 0, 10))   # fmv.x.w a0, f3
    asm.emit("li32", "t1", 0x40700000)
    asm.emit("beq", "a0", "t1", "ok_add")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_add")
    asm.label("ok_add")
    asm.emit("li", "s1", ord('F'))
    asm.label("print_add")
    asm.emit("sb", "s1", "s2", 0)

    # ---- fmul 1.5 * 2.25 = 3.375 ----
    asm.emit("word", fp(0x08, 2, 1, 0, 4))
    asm.emit("word", fp(0x70, 0, 4, 0, 10))
    asm.emit("li32", "t1", 0x40580000)
    asm.emit("beq", "a0", "t1", "ok_mul")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_mul")
    asm.label("ok_mul")
    asm.emit("li", "s1", ord('F'))
    asm.label("print_mul")
    asm.emit("sb", "s1", "s2", 0)

    # ---- fsqrt 9.0 = 3.0 ----
    asm.emit("word", fp(0x2C, 0, 6, 0, 5))
    asm.emit("word", fp(0x70, 0, 5, 0, 10))
    asm.emit("li32", "t1", 0x40400000)
    asm.emit("beq", "a0", "t1", "ok_sqrt")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_sqrt")
    asm.label("ok_sqrt")
    asm.emit("li", "s1", ord('F'))
    asm.label("print_sqrt")
    asm.emit("sb", "s1", "s2", 0)

    # ---- fcvt.w.s (RTZ): 3.0 -> 3 ----
    asm.emit("word", fp(0x60, 0, 5, 1, 10))
    asm.emit("li", "t1", 3)
    asm.emit("beq", "a0", "t1", "ok_cvt")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_cvt")
    asm.label("ok_cvt")
    asm.emit("li", "s1", ord('F'))
    asm.label("print_cvt")
    asm.emit("sb", "s1", "s2", 0)

    # ---- fsw 往返: 3.75f 写回内存, 用 lw 读原始位 ----
    asm.emit("word", fsw(12, 3, 19))     # fsw f3, 12(s3)
    asm.emit("lw", "a0", "s3", 12)
    asm.emit("li32", "t1", 0x40700000)
    asm.emit("beq", "a0", "t1", "ok_store")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_store")
    asm.label("ok_store")
    asm.emit("li", "s1", ord('F'))
    asm.label("print_store")
    asm.emit("sb", "s1", "s2", 0)

    # 换行 + exit(0)
    asm.emit("li", "s1", 10)
    asm.emit("sb", "s1", "s2", 0)
    asm.emit("li", "a7", 93)
    asm.emit("li", "a0", 0)
    asm.emit("ecall")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/f_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
