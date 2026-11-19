#!/usr/bin/env python3
"""
生成一个 PLIC 外部中断演示 ELF (v0.14)。

流程:

  1. 配置 3 条中断源: IRQ1 优先级 1, IRQ2 优先级 3, IRQ3 优先级 2
  2. 使能这 3 条中断, 阈值设为 0, 打开 mie.MEIE / mstatus.MIE
  3. 往虚拟设备的门铃寄存器写 0b1110 —— 设备同时拉起 3 条中断线
  4. 下一条指令边界进 trap (mcause = 0x8000000B)
  5. handler 循环 claim: 按优先级拿到 2 → 3 → 1, 每条处理完 complete
  6. claim 返回 0 表示处理完, 置 DONE, mret 回到主流程
  7. 主流程打印 claim 顺序 "PLIC:231"

关键点:
  - PLIC 的 pending 是设备拉起来的, 软件只能通过 claim 清掉
  - 优先级数字大的先报; 同优先级取编号小的
  - 阈值是"严格大于": threshold=0 时优先级 0 的源永远不报
  - 写 claim 地址 = complete, 告诉 PLIC 这条中断处理完了

用法:
    python tests/gen_plic_demo.py build/plic_demo.elf
    build/rvemu build/plic_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE, DATA

PRIO_BASE = 0x0C000000          # 优先级: +4*irq
ENABLE0   = 0x0C002000          # hart0 的使能字
THRESH0   = 0x0C200000          # hart0 阈值
CLAIM0    = 0x0C200004          # hart0 claim/complete
DOORBELL  = 0x10000000          # 虚拟设备门铃

DONE      = DATA + 0x00
BUF       = DATA + 0x80


def build():
    asm = Assembler()

    asm.label("_start")
    asm.emit("li32", "t0", "handler")
    asm.emit("csrw", 0x305, "t0")            # mtvec = handler

    # 优先级: IRQ1=1, IRQ2=3, IRQ3=2
    asm.emit("li32", "t0", PRIO_BASE + 4)
    asm.emit("li", "t1", 1)
    asm.emit("sw", "t1", "t0", 0)
    asm.emit("li32", "t0", PRIO_BASE + 8)
    asm.emit("li", "t1", 3)
    asm.emit("sw", "t1", "t0", 0)
    asm.emit("li32", "t0", PRIO_BASE + 12)
    asm.emit("li", "t1", 2)
    asm.emit("sw", "t1", "t0", 0)

    # 使能 IRQ1/2/3, 阈值 0
    asm.emit("li32", "t0", ENABLE0)
    asm.emit("li", "t1", 0b1110)
    asm.emit("sw", "t1", "t0", 0)
    asm.emit("li32", "t0", THRESH0)
    asm.emit("sw", "zero", "t0", 0)

    # mie.MEIE = 1<<11; mstatus.MIE = 1<<3
    asm.emit("li", "t0", 1 << 11)
    asm.emit("csrs", 0x304, "t0")
    asm.emit("li", "t0", 1 << 3)
    asm.emit("csrs", 0x300, "t0")

    # 设备同时拉起 3 条线
    asm.emit("li32", "t0", DOORBELL)
    asm.emit("li", "t1", 0b1110)
    asm.emit("sw", "t1", "t0", 0)

    asm.label("wait")
    asm.emit("li32", "t1", DONE)
    asm.emit("lw", "t2", "t1", 0)
    asm.emit("beq", "t2", "zero", "wait")

    # 打印前关掉 trap: 本模拟器 mtvec=0 时 ecall 才走宿主系统调用
    asm.emit("csrw", 0x305, "zero")

    # 输出 "PLIC:" + handler 记下的 3 个字符
    asm.emit("li", "a0", 1)
    asm.emit("li32", "a1", "tag")
    asm.emit("li", "a2", 5)
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    asm.emit("li", "a0", 1)
    asm.emit("li32", "a1", BUF)
    asm.emit("li", "a2", 3)
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    asm.emit("li", "a0", 1)
    asm.emit("li32", "a1", "nl")
    asm.emit("li", "a2", 1)
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    asm.emit("li", "a0", 0)
    asm.emit("li", "a7", 93)
    asm.emit("ecall")

    # ---------------- PLIC handler ----------------
    asm.label("handler")
    asm.emit("li32", "t3", CLAIM0)
    asm.emit("li32", "t4", BUF)

    asm.label("claim_loop")
    asm.emit("lw", "t0", "t3", 0)            # claim
    asm.emit("beq", "t0", "zero", "hdl_done")
    asm.emit("sw", "t0", "t3", 0)            # complete
    asm.emit("addi", "t0", "t0", 48)         # IRQ 号 → ASCII
    asm.emit("sb", "t0", "t4", 0)
    asm.emit("addi", "t4", "t4", 1)
    asm.emit("j", "claim_loop")

    asm.label("hdl_done")
    asm.emit("li32", "t5", DONE)
    asm.emit("li", "t6", 1)
    asm.emit("sw", "t6", "t5", 0)
    asm.emit("mret")

    asm.label("tag")
    asm.data(b"PLIC:")
    asm.label("nl")
    asm.data(b"\n")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "plic_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
