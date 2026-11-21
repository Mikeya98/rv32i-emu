#!/usr/bin/env python3
"""
生成一个 S 态外部中断 + WFI 演示 ELF (v0.15)。

流程 (两核):

  hart0 (主核):
    1. M 态配置: stvec = S handler, mideleg[9] = 1 (S 外部中断委托给 S)
    2. 配置 PLIC 的 S 上下文 (hart0 是 ctx1): 使能 IRQ2、优先级 3、阈值 0
    3. sie.SEIE = 1, sstatus.SIE = 1, mret 进 S 态
    4. S 态打印 'S', 然后执行 WFI 等中断
    5. hart1 在门铃上拉起 IRQ2 → SEIP → WFI 醒来 → trap 到 stvec
    6. S handler 从 PLIC ctx1 claim IRQ2, 打印 ':', complete, sret
    7. 回到 WFI 之后, 打印换行并停机

  hart1 (辅助核):
    空转一小段时间, 往虚拟设备门铃写 bit2, 然后停机。

预期输出:
    S:2

关键点:
  - 每个 hart 有两个 PLIC 上下文: M = 2h, S = 2h+1
  - M 上下文拉 mip.MEIP; S 上下文拉 mip.SEIP (sip.SEIP 是 S 态视图)
  - mideleg[9] = 1 时 S 外部中断直接进 stvec; 否则由 M 态用 mcause=9 处理
  - WFI 在"已使能的 pending 中断"到来时醒来, 不看全局中断开关

用法:
    python tests/gen_plic_s_demo.py build/plic_s_demo.elf
    build/rvemu --harts 2 build/plic_s_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

PRIO_IRQ2 = 0x0C000000 + 8      # IRQ2 的优先级寄存器
ENABLE_S0 = 0x0C002000 + 0x80   # hart0 S 上下文 (ctx1) 的使能字
THRESH_S0 = 0x0C200000 + 0x1000 # hart0 S 上下文阈值
CLAIM_S0  = THRESH_S0 + 4       # hart0 S 上下文 claim/complete
DOORBELL  = 0x10000000
UART      = 0x20000000

STVEC   = 0x105
SIE     = 0x104
SSTATUS = 0x100
MIDELEG = 0x303
MEPC    = 0x341
MSTATUS = 0x300


def build():
    asm = Assembler()

    asm.label("_start")
    asm.emit("csrr", "t0", 0xF14)            # mhartid
    asm.emit("bnez", "t0", "hart1")

    # ---------------- hart0: M 态配置 ----------------
    asm.emit("li32", "t0", "s_handler")
    asm.emit("csrw", STVEC, "t0")

    asm.emit("li", "t0", 1 << 9)             # mideleg[9] = SEIP
    asm.emit("csrw", MIDELEG, "t0")

    asm.emit("li32", "t0", PRIO_IRQ2)
    asm.emit("li", "t1", 3)
    asm.emit("sw", "t1", "t0", 0)

    asm.emit("li32", "t0", ENABLE_S0)
    asm.emit("li", "t1", 1 << 2)
    asm.emit("sw", "t1", "t0", 0)

    asm.emit("li32", "t0", THRESH_S0)
    asm.emit("sw", "zero", "t0", 0)

    # S 态中断: sie.SEIE = 1
    asm.emit("li", "t0", 1 << 9)
    asm.emit("csrw", SIE, "t0")

    # mret 进 S 态: MPP = 01, 同时带上 sstatus.SIE (bit1)
    # 注意顺序: mstatus 最后写, 否则会把前面设置的 SIE 覆盖掉
    asm.emit("li32", "t0", "s_main")
    asm.emit("csrw", MEPC, "t0")
    asm.emit("li32", "t0", (1 << 11) | (1 << 1))
    asm.emit("csrw", MSTATUS, "t0")
    asm.emit("mret")

    # ---------------- S 态主流程 ----------------
    asm.label("s_main")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('S'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("word", 0x10500073)             # wfi: 等 hart1 拉门铃

    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('\n'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("ebreak")                       # mtvec = 0 → 停机

    # ---------------- S 态 handler ----------------
    asm.label("s_handler")
    asm.emit("li32", "t4", UART)
    asm.emit("li", "t1", ord(':'))
    asm.emit("sb", "t1", "t4", 0)

    asm.emit("li32", "t3", CLAIM_S0)
    asm.emit("lw", "t0", "t3", 0)            # claim
    asm.emit("beq", "t0", "zero", "s_done")
    asm.emit("sw", "t0", "t3", 0)            # complete
    asm.emit("addi", "t0", "t0", 48)
    asm.emit("sb", "t0", "t4", 0)

    asm.label("s_done")
    asm.emit("sret")

    # ---------------- hart1: 稍后拉门铃 ----------------
    asm.label("hart1")
    asm.emit("li", "t2", 40)

    asm.label("h1_loop")
    asm.emit("addi", "t2", "t2", -1)
    asm.emit("bnez", "t2", "h1_loop")

    asm.emit("li32", "t0", DOORBELL)
    asm.emit("li", "t1", 1 << 2)
    asm.emit("sw", "t1", "t0", 0)
    asm.emit("ebreak")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "plic_s_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
