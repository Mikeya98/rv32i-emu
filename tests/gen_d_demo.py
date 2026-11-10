#!/usr/bin/env python3
"""
生成一个演示 RV32D 双精度浮点的 ELF。

程序流程:
    1. 把 1.5 / 2.25 / 9.0 写成 64 位立即数放进内存, 用 fld 读进 f 寄存器
    2. fadd.d 1.5 + 2.25 = 3.75
    3. fmul.d 1.5 * 2.25 = 3.375
    4. fsqrt.d 9.0 = 3.0
    5. fcvt.w.d (RTZ) 3.75 = 3
    6. fcvt.s.d 把 1.5 收窄成单精度, 再 fcvt.d.s 加宽回来
    7. fmadd/fmsub/fnmsub/fnmadd.d: 1.5*2.25 ± 5.0
    每项成功打印一个 'D', 失败打印 'X'

预期输出:
    DDDDDDDDDD

用法:
    python tests/gen_d_demo.py build/d_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

UART = 0x20000000
DATA_ADDR = 0x12000


def fp(f7, rs2, rs1, f3, rd):
    return ((f7 & 0x7F) << 25) | ((rs2 & 0x1F) << 20) | \
           ((rs1 & 0x1F) << 15) | ((f3 & 7) << 12) | \
           ((rd & 0x1F) << 7) | 0x53


def fld(imm, rs1, fd):
    return ((imm & 0xFFF) << 20) | ((rs1 & 0x1F) << 15) | \
           (3 << 12) | ((fd & 0x1F) << 7) | 0x07


def fsd(imm, fs2, rs1):
    return (((imm >> 5) & 0x7F) << 25) | ((fs2 & 0x1F) << 20) | \
           ((rs1 & 0x1F) << 15) | (3 << 12) | \
           ((imm & 0x1F) << 7) | 0x27


def fma(opcode, fmt, rs3, rs2, rs1, rm, rd):
    return ((rs3 & 0x1F) << 27) | ((fmt & 3) << 25) | \
           ((rs2 & 0x1F) << 20) | ((rs1 & 0x1F) << 15) | \
           ((rm & 7) << 12) | ((rd & 0x1F) << 7) | opcode


FMADD, FMSUB, FNMSUB, FNMADD = 0x43, 0x47, 0x4B, 0x4F


def build():
    asm = Assembler()
    asm.label("_start")

    asm.emit("li32", "s2", UART)
    asm.emit("li32", "s3", DATA_ADDR)

    # mem[0..7]   = 1.5   (0x3FF8000000000000)
    # mem[8..15]  = 2.25  (0x4002000000000000)
    # mem[16..23] = 9.0   (0x4022000000000000)
    # mem[40..47] = 5.0   (0x4014000000000000)
    for off, hi in ((0, 0x3FF80000), (8, 0x40020000),
                    (16, 0x40220000), (40, 0x40140000)):
        asm.emit("li32", "t0", hi)
        asm.emit("sw", "t0", "s3", off + 4)     # 高 32 位
        asm.emit("li32", "t0", 0)
        asm.emit("sw", "t0", "s3", off)         # 低 32 位

    asm.emit("word", fld(0, 19, 1))      # fld f1, 0(s3)
    asm.emit("word", fld(8, 19, 2))      # fld f2, 8(s3)
    asm.emit("word", fld(16, 19, 6))     # fld f6, 16(s3)
    asm.emit("word", fld(40, 19, 7))     # fld f7, 40(s3)

    def check(expr_hi, label):
        """把 f3 写回内存高半字, 和期望值比较, 打印 F/D。"""
        asm.emit("word", fsd(24, 3, 19))          # fsd f3, 24(s3)
        asm.emit("lw", "a0", "s3", 28)            # 高 32 位
        asm.emit("li32", "t1", expr_hi)
        asm.emit("beq", "a0", "t1", "ok_" + label)
        asm.emit("li", "s1", ord('X'))
        asm.emit("j", "print_" + label)
        asm.label("ok_" + label)
        asm.emit("li", "s1", ord('D'))
        asm.label("print_" + label)
        asm.emit("sb", "s1", "s2", 0)

    # ---- fadd.d 1.5 + 2.25 = 3.75 ----
    asm.emit("word", fp(0x01, 2, 1, 0, 3))
    check(0x400E0000, "add")

    # ---- fmul.d 1.5 * 2.25 = 3.375 ----
    asm.emit("word", fp(0x09, 2, 1, 0, 3))
    check(0x400B0000, "mul")

    # ---- fsqrt.d 9.0 = 3.0 ----
    asm.emit("word", fp(0x2D, 0, 6, 0, 3))
    check(0x40080000, "sqrt")

    # ---- fcvt.w.d (RTZ): f3 里是 sqrt 得到的 3.0 -> 3 ----
    asm.emit("word", fp(0x61, 0, 3, 1, 10))
    asm.emit("li", "t1", 3)
    asm.emit("beq", "a0", "t1", "ok_cvt")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_cvt")
    asm.label("ok_cvt")
    asm.emit("li", "s1", ord('D'))
    asm.label("print_cvt")
    asm.emit("sb", "s1", "s2", 0)

    # ---- 收窄再放宽: fcvt.s.d 1.5 -> 1.5f, fcvt.d.s 回来 ----
    asm.emit("word", fp(0x20, 1, 1, 0, 4))    # fcvt.s.d f4, f1
    asm.emit("word", fp(0x21, 0, 4, 0, 3))    # fcvt.d.s f3, f4
    check(0x3FF80000, "roundtrip")

    # ---- FLD 原始位检查: 1.5 的低 32 位应为 0 ----
    asm.emit("word", fsd(32, 1, 19))          # fsd f1, 32(s3)
    asm.emit("lw", "a0", "s3", 32)
    asm.emit("beq", "zero", "a0", "ok_bits")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_bits")
    asm.label("ok_bits")
    asm.emit("li", "s1", ord('D'))
    asm.label("print_bits")
    asm.emit("sb", "s1", "s2", 0)

    # ---- 融合乘加: 1.5*2.25 = 3.375, 加减 5.0 ----
    asm.emit("word", fma(FMADD,  1, 7, 2, 1, 0, 3))
    check(0x4020C000, "fmadd")       # 8.375
    asm.emit("word", fma(FMSUB,  1, 7, 2, 1, 0, 3))
    check(0xBFFA0000, "fmsub")       # -1.625
    asm.emit("word", fma(FNMSUB, 1, 7, 2, 1, 0, 3))
    check(0x3FFA0000, "fnmsub")      # 1.625
    asm.emit("word", fma(FNMADD, 1, 7, 2, 1, 0, 3))
    check(0xC020C000, "fnmadd")      # -8.375

    # 换行 + exit(0)
    asm.emit("li", "s1", 10)
    asm.emit("sb", "s1", "s2", 0)
    asm.emit("li", "a7", 93)
    asm.emit("li", "a0", 0)
    asm.emit("ecall")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/d_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
