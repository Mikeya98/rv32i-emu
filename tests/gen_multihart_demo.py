#!/usr/bin/env python3
"""
生成一个双 hart 演示 ELF。

两个 hart 从同一个入口开始执行, 各自读 mhartid 分流:

  hart0: 用 amoadd.w 把共享计数器加到 20, 然后自旋等 hart1 的标志位,
         读到 50 之后打印 "MH50" 并退出
  hart1: 用 amoadd.w 再把计数器加到 50, 置标志位, 退出

关键点:
  - 两个 hart 各有自己的寄存器, 共享同一块内存
  - 计数器用 AMO 而不是 "lw/add/sw", 否则双核下会丢更新
  - 标志位用一个普通 store 写, 轮转执行下顺序是确定的

用法:
    python tests/gen_multihart_demo.py build/mh_demo.elf
    build/rvemu --harts 2 build/mh_demo.elf
"""

import struct
import sys

from gen_demo_elf import Assembler, write_elf, BASE, DATA


COUNTER = DATA + 0x00        # 0x11000
FLAG    = DATA + 0x04        # 0x11004
BUF_END = DATA + 0x80        # 打印缓冲区尾
H0_LOOPS = 20
H1_LOOPS = 30


def build():
    asm = Assembler()

    asm.label("_start")
    asm.emit("csrr", "a0", 0xF14)          # a0 = mhartid
    asm.emit("bnez", "a0", "hart1")

    # ---------------- hart0 ----------------
    asm.label("hart0")
    asm.emit("li", "t0", H0_LOOPS)
    asm.emit("li32", "t1", COUNTER)
    asm.emit("li", "t3", 1)

    asm.label("h0_loop")
    asm.emit("amoadd.w", "zero", "t3", "t1")
    asm.emit("addi", "t0", "t0", -1)
    asm.emit("bnez", "t0", "h0_loop")

    # 等 hart1 把标志置上
    asm.label("h0_wait")
    asm.emit("li32", "t2", FLAG)
    asm.emit("lw", "t4", "t2", 0)
    asm.emit("beq", "t4", "zero", "h0_wait")

    # a0 = 计数器最终值, 转十进制打印
    asm.emit("lw", "a0", "t1", 0)
    asm.emit("li32", "t2", BUF_END)
    asm.emit("mv", "t3", "t2")
    asm.emit("li", "t4", 10)

    asm.label("digit_loop")
    asm.emit("remu", "t5", "a0", "t4")
    asm.emit("divu", "a0", "a0", "t4")
    asm.emit("addi", "t5", "t5", 48)
    asm.emit("addi", "t3", "t3", -1)
    asm.emit("sb", "t5", "t3", 0)
    asm.emit("bnez", "a0", "digit_loop")

    # 在数字前面补 "MH"
    asm.emit("addi", "t3", "t3", -2)
    asm.emit("li", "t5", ord("M"))
    asm.emit("sb", "t5", "t3", 0)
    asm.emit("li", "t5", ord("H"))
    asm.emit("sb", "t5", "t3", 1)

    asm.emit("li", "a0", 1)
    asm.emit("mv", "a1", "t3")
    asm.emit("sub", "a2", "t2", "t3")
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    asm.emit("li32", "a1", "nl")
    asm.emit("li", "a0", 1)
    asm.emit("li", "a2", 1)
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    asm.emit("li", "a0", 0)
    asm.emit("li", "a7", 93)
    asm.emit("ecall")

    # ---------------- hart1 ----------------
    asm.label("hart1")
    asm.emit("li", "t0", H1_LOOPS)
    asm.emit("li32", "t1", COUNTER)
    asm.emit("li", "t3", 1)

    asm.label("h1_loop")
    asm.emit("amoadd.w", "zero", "t3", "t1")
    asm.emit("addi", "t0", "t0", -1)
    asm.emit("bnez", "t0", "h1_loop")

    asm.emit("li32", "t2", FLAG)
    asm.emit("li", "t4", 1)
    asm.emit("sw", "t4", "t2", 0)

    asm.emit("li", "a0", 0)
    asm.emit("li", "a7", 93)
    asm.emit("ecall")

    asm.label("nl")
    asm.data(b"\n")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "mh_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
