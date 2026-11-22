#!/usr/bin/env python3
"""
生成一个电平设备中断演示 ELF (v0.17)。

流程 (单核):

  1. 配置 IRQ5: 优先级 4, 使能, 阈值 0; 打开 mie.MEIE / mstatus.MIE
  2. 往电平设备寄存器写 1<<5, 把 IRQ5 的线拉高
  3. 进入 handler: claim → tick++ → 打印序号 → complete
  4. 前两次 complete 时线还是高的, PLIC 立刻重新置 pending,
     下一条指令又进 handler —— 这就是电平设备的中断重报
  5. 第 3 次先把设备线写成 0, 再 complete, 中断停下来
  6. 主流程打印换行, 停机

预期输出:
    L123

关键点:
  - 电平设备的 pending 由线电平决定, complete 之后线还高就会重报
  - 驱动必须在 complete 之前把设备的中断清掉, 顺序反了就是中断风暴
  - 边沿设备 (门铃) 没有线电平, complete 后不会重报

用法:
    python tests/gen_level_demo.py build/level_demo.elf
    build/rvemu build/level_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE, DATA

UART      = 0x20000000
PRIO5     = 0x0C000000 + 4 * 5
ENABLE0   = 0x0C002000
THRESH0   = 0x0C200000
CLAIM0    = 0x0C200004
DEV_LEVEL = 0x10000004          # 电平设备: bit n = 线 n 的电平

TICK      = DATA + 0x00
DONE      = DATA + 0x04

MTVEC     = 0x305
MSTATUS   = 0x300
MIE       = 0x304
MEPC      = 0x341


def build():
    asm = Assembler()

    asm.label("_start")

    # 先打印 'L' —— 这时 mtvec 还是 0, 用 UART 直接写
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('L'))
    asm.emit("sb", "t1", "t0", 0)

    # 中断配置
    asm.emit("li32", "t0", "handler")
    asm.emit("csrw", MTVEC, "t0")

    asm.emit("li32", "t0", PRIO5)
    asm.emit("li", "t1", 4)
    asm.emit("sw", "t1", "t0", 0)

    asm.emit("li32", "t0", ENABLE0)
    asm.emit("li", "t1", 1 << 5)
    asm.emit("sw", "t1", "t0", 0)

    asm.emit("li32", "t0", THRESH0)
    asm.emit("sw", "zero", "t0", 0)

    asm.emit("li", "t0", 1 << 11)            # mie.MEIE
    asm.emit("csrs", MIE, "t0")
    asm.emit("li", "t0", 1 << 3)             # mstatus.MIE
    asm.emit("csrs", MSTATUS, "t0")

    # 电平设备: 拉高 IRQ5
    asm.emit("li32", "t0", DEV_LEVEL)
    asm.emit("li", "t1", 1 << 5)
    asm.emit("sw", "t1", "t0", 0)

    # 等 handler 处理完
    asm.label("wait")
    asm.emit("li32", "t1", DONE)
    asm.emit("lw", "t2", "t1", 0)
    asm.emit("beq", "t2", "zero", "wait")

    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('\n'))
    asm.emit("sb", "t1", "t0", 0)

    # 关掉 mtvec 再 ebreak, 否则会跳进 handler
    asm.emit("csrw", MTVEC, "zero")
    asm.emit("ebreak")

    # ---------------- PLIC handler ----------------
    asm.label("handler")
    asm.emit("li32", "t3", CLAIM0)
    asm.emit("lw", "t0", "t3", 0)            # claim → IRQ 号
    asm.emit("beq", "t0", "zero", "h_ret")   # 没有中断就直接返回

    # tick++
    asm.emit("li32", "t4", TICK)
    asm.emit("lw", "t5", "t4", 0)
    asm.emit("addi", "t5", "t5", 1)
    asm.emit("sw", "t5", "t4", 0)

    # 打印序号 (t5 保留计数, 用 t2 转 ASCII)
    asm.emit("li32", "t6", UART)
    asm.emit("addi", "t2", "t5", 48)
    asm.emit("sb", "t2", "t6", 0)

    # 第 3 次: 先把设备的中断清掉 —— 必须在 complete 之前
    asm.emit("li", "t1", 3)
    asm.emit("blt", "t5", "t1", "h_complete")
    asm.emit("li32", "t1", DEV_LEVEL)
    asm.emit("sw", "zero", "t1", 0)
    asm.emit("li32", "t1", DONE)
    asm.emit("li", "t2", 1)
    asm.emit("sw", "t2", "t1", 0)

    asm.label("h_complete")
    asm.emit("sw", "t0", "t3", 0)            # complete

    asm.label("h_ret")
    asm.emit("mret")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "level_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
