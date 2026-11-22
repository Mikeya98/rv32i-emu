#!/usr/bin/env python3
"""
生成一个 S 态定时器中断 (Sstc) 演示 ELF (v0.16)。

流程 (单核):

  M 态:
    1. stvec = S handler, mideleg[5] = 1 (STIP 委托给 S 态)
    2. sie.STIE = 1, stimecmp = 40
    3. mstatus = SPP + SIE, mret 进 S 态

  S 态:
    4. 打印 'S', 执行 WFI 等定时器
    5. 每次 STIP 进 handler: 打印滴答序号, stimecmp += 20
    6. 第 3 次之后关掉定时器, sret 回到 WFI 之后
    7. 打印换行, ebreak 停机

预期输出:
    S123

关键点:
  - STIP 是电平信号, stimecmp 到点后一直挂着, 直到软件改比较值
  - mideleg[5] = 1 时 S 定时器中断直接进 stvec (scause 0x80000005)
  - WFI 在"已使能的 pending 中断"到来时醒来, 不看 mstatus.SIE
  - S 态只能写 stimecmp (0x14D/0x15D), 不能写 sip.STIP

用法:
    python tests/gen_stip_demo.py build/stip_demo.elf
    build/rvemu build/stip_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE, DATA

UART = 0x20000000

STVEC     = 0x105
SIE       = 0x104
MIDELEG   = 0x303
MEPC      = 0x341
MSTATUS   = 0x300
STIMECMP  = 0x14D      # Sstc: S 态定时器比较值 (低 32 位)


def build():
    asm = Assembler()

    asm.label("_start")

    # ---------------- M 态配置 ----------------
    asm.emit("li32", "t0", "s_handler")
    asm.emit("csrw", STVEC, "t0")

    asm.emit("li", "t0", 1 << 5)             # mideleg[5] = STIP
    asm.emit("csrw", MIDELEG, "t0")

    asm.emit("li", "t0", 1 << 5)             # sie.STIE
    asm.emit("csrw", SIE, "t0")

    asm.emit("li", "t0", 40)                 # 第一次定时器: mtime = 40
    asm.emit("csrw", STIMECMP, "t0")

    asm.emit("li32", "t0", "s_main")
    asm.emit("csrw", MEPC, "t0")

    # mret 进 S 态, 同时带上 sstatus.SIE。
    # 必须用 li32: 0x802 的 bit11 是 1, li (addi) 会符号扩展成 0xFFFFF802,
    # MPP 变成 3, mret 就回到 M 态了 —— v0.15 踩过同一个坑。
    asm.emit("li32", "t0", (1 << 11) | (1 << 1))
    asm.emit("csrw", MSTATUS, "t0")
    asm.emit("mret")

    # ---------------- S 态主流程 ----------------
    asm.label("s_main")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('S'))
    asm.emit("sb", "t1", "t0", 0)

    # 每醒一次就回去接着 WFI, 直到 handler 第 3 次把 sepc 改成 s_exit
    asm.label("s_wait")
    asm.emit("word", 0x10500073)             # wfi: 等 STIP
    asm.emit("j", "s_wait")

    asm.label("s_exit")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('\n'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("ebreak")                       # mtvec = 0 → 停机

    # ---------------- S 态定时器 handler ----------------
    asm.label("s_handler")
    asm.emit("li32", "t4", UART)

    # tick++
    asm.emit("li32", "t0", DATA)
    asm.emit("lw", "t1", "t0", 0)
    asm.emit("addi", "t1", "t1", 1)
    asm.emit("sw", "t1", "t0", 0)

    # 打印 '1' / '2' / '3'
    asm.emit("addi", "t2", "t1", 48)
    asm.emit("sb", "t2", "t4", 0)

    # 下一次定时器: stimecmp += 20
    asm.emit("csrr", "t2", STIMECMP)
    asm.emit("addi", "t2", "t2", 20)
    asm.emit("csrw", STIMECMP, "t2")

    # 第 3 次之后关掉定时器, 并让 sret 直接回到 s_exit
    asm.emit("li", "t3", 3)
    asm.emit("bne", "t1", "t3", "s_ret")
    asm.emit("csrw", STIMECMP, "zero")
    asm.emit("li32", "t3", "s_exit")
    asm.emit("csrw", 0x141, "t3")            # sepc = s_exit

    asm.label("s_ret")
    asm.emit("sret")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "stip_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
