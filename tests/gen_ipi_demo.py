#!/usr/bin/env python3
"""
生成一个核间中断 (IPI) 演示 ELF。

两个 hart 从同一个入口启动, 用 mhartid 分流:

  hart0: 装 mtvec + 打开 MSIE/MIE, 然后自旋等 DONE 标志
  hart1: 用 amoadd.w 把 10+9+...+1 累加到共享内存, 然后写 CLINT msip
         给 hart0 发一个软件中断, 自己退出
  hart0 的 handler: 写 0 清掉自己的 msip (电平信号不清会反复进中断),
                    再置 DONE。中断返回后 hart0 打印 "IPI55"

关键点:
  - CLINT msip 是每个 hart 一个 32 位寄存器, 写 1 抬电平, 写 0 落下
  - 软件中断走 mcause = 0x80000003, 和定时器中断 (0x80000007) 分开
  - 电平中断必须在 handler 里清掉, 否则 mret 一回去又立刻进一次

用法:
    python tests/gen_ipi_demo.py build/ipi_demo.elf
    build/rvemu --harts 2 build/ipi_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE, DATA


SUM     = DATA + 0x00        # hart1 累加结果
DONE    = DATA + 0x04        # handler 置 1
BUF_END = DATA + 0x80        # 打印缓冲区尾
MSIP_H0 = 0x02000000         # hart0 的 msip


def build():
    asm = Assembler()

    asm.label("_start")
    asm.emit("csrr", "a0", 0xF14)          # a0 = mhartid
    asm.emit("bnez", "a0", "hart1")

    # ---------------- hart0 ----------------
    asm.emit("li32", "t0", "handler")
    asm.emit("csrw", 0x305, "t0")          # mtvec = handler
    asm.emit("li", "t0", 8)
    asm.emit("csrs", 0x304, "t0")          # mie.MSIE
    asm.emit("li", "t0", 8)
    asm.emit("csrs", 0x300, "t0")          # mstatus.MIE

    asm.label("h0_wait")
    asm.emit("li32", "t1", DONE)
    asm.emit("lw", "t2", "t1", 0)
    asm.emit("beq", "t2", "zero", "h0_wait")

    # 打印前先关掉 trap: 本模拟器的 ecall 在 mtvec=0 时走宿主系统调用,
    # 装了 mtvec 之后 ecall 会被当成普通 trap 进 handler。
    asm.emit("csrw", 0x305, "zero")

    # 打印 "IPI" + SUM 的十进制值
    asm.emit("li32", "t1", SUM)
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

    # 前面补 "IPI"
    asm.emit("addi", "t3", "t3", -3)
    asm.emit("li", "t5", ord("I"))
    asm.emit("sb", "t5", "t3", 0)
    asm.emit("li", "t5", ord("P"))
    asm.emit("sb", "t5", "t3", 1)
    asm.emit("li", "t5", ord("I"))
    asm.emit("sb", "t5", "t3", 2)

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
    asm.emit("li", "t0", 10)
    asm.emit("li32", "t1", SUM)

    asm.label("h1_loop")
    asm.emit("amoadd.w", "zero", "t0", "t1")
    asm.emit("addi", "t0", "t0", -1)
    asm.emit("bnez", "t0", "h1_loop")

    # 给 hart0 发 IPI
    asm.emit("li32", "t1", MSIP_H0)
    asm.emit("li", "t2", 1)
    asm.emit("sw", "t2", "t1", 0)

    asm.emit("li", "a0", 0)
    asm.emit("li", "a7", 93)
    asm.emit("ecall")

    # ---------------- hart0 的软件中断 handler ----------------
    asm.label("handler")
    asm.emit("li32", "t1", MSIP_H0)
    asm.emit("sw", "zero", "t1", 0)        # 清 msip, 否则会反复进中断
    asm.emit("li32", "t1", DONE)
    asm.emit("li", "t2", 1)
    asm.emit("sw", "t2", "t1", 0)
    asm.emit("mret")

    asm.label("nl")
    asm.data(b"\n")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "ipi_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
