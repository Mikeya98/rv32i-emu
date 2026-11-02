#!/usr/bin/env python3
"""
生成一个演示 RV32A（LR/SC + AMO）的 RV32IM ELF。

程序流程:
    1. mem[0x11000] = 5
    2. lr.w / sc.w 成功写入 7      → 打印 'S'
    3. lr.w 之后用普通 sw 打断保留集，再 sc.w 失败 → 打印 'R'
    4. amoadd.w 返回旧值 9，mem 变成 16 → 打印 'A'
    5. amomaxu.w 返回旧值 16（max(16,7)=16）→ 打印 'M'
    6. 换行并 exit(0)

预期输出:
    SRAM

用法:
    python tests/gen_atomic_demo.py build/atomic_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

UART = 0x20000000
DATA_ADDR = 0x11000


def build():
    asm = Assembler()
    asm.label("_start")

    # 数据区初始化：mem[DATA_ADDR] = 5
    asm.emit("li32", "t0", DATA_ADDR)
    asm.emit("li", "t1", 5)
    asm.emit("sw", "t1", "t0", 0)

    # ---- LR / SC 成功 ----
    asm.emit("lr.w", "t2", "t0")
    asm.emit("li", "t1", 7)
    asm.emit("sc.w", "s0", "t1", "t0")
    asm.emit("beq", "s0", "zero", "sc_ok")
    asm.emit("li", "s1", ord('F'))
    asm.emit("j", "print_sc")
    asm.label("sc_ok")
    asm.emit("li", "s1", ord('S'))
    asm.label("print_sc")
    asm.emit("li32", "s2", UART)
    asm.emit("sb", "s1", "s2", 0)

    # ---- 普通 store 打断保留集，SC 必须失败 ----
    asm.emit("lr.w", "s3", "t0")
    asm.emit("li", "s4", 9)
    asm.emit("sw", "s4", "t0", 0)
    asm.emit("sc.w", "s5", "t1", "t0")
    asm.emit("li", "s6", 1)
    asm.emit("beq", "s5", "s6", "res_ok")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_res")
    asm.label("res_ok")
    asm.emit("li", "s1", ord('R'))
    asm.label("print_res")
    asm.emit("sb", "s1", "s2", 0)

    # ---- amoadd.w：旧值 9，mem 变成 16 ----
    asm.emit("amoadd.w", "s7", "t1", "t0")
    asm.emit("li", "s8", 9)
    asm.emit("beq", "s7", "s8", "add_ok")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_add")
    asm.label("add_ok")
    asm.emit("li", "s1", ord('A'))
    asm.label("print_add")
    asm.emit("sb", "s1", "s2", 0)

    # ---- amomaxu.w：旧值 16，max(16, 7) = 16 ----
    asm.emit("amomaxu.w", "s9", "t1", "t0")
    asm.emit("li", "s10", 16)
    asm.emit("beq", "s9", "s10", "max_ok")
    asm.emit("li", "s1", ord('X'))
    asm.emit("j", "print_max")
    asm.label("max_ok")
    asm.emit("li", "s1", ord('M'))
    asm.label("print_max")
    asm.emit("sb", "s1", "s2", 0)

    # 换行 + exit(0)
    asm.emit("li", "s1", 10)
    asm.emit("sb", "s1", "s2", 0)
    asm.emit("li", "a7", 93)
    asm.emit("li", "a0", 0)
    asm.emit("ecall")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/atomic_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
