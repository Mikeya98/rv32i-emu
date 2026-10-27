#!/usr/bin/env python3
"""
生成一个演示 trap 框架的 RV32IM ELF。

程序流程:
    1. 装 mtvec 指向 handler
    2. 连续执行 3 次 ecall —— 每次都陷入 handler, 打印一个 '.', 然后
       mepc += 4 跳过 ecall 指令 mret 回来
    3. 打开机器定时器中断 (mie.MTIE + mstatus.MIE), 忙等 3 次 tick,
       handler 每次打印一个 'T' 并把 mtimecmp 往后推 10
    4. 关中断, 把 mtvec 清零, 最后用 ecall 退出 (mtvec=0 时走宿主
       系统调用路径)

预期输出:
    ...TTT

用法:
    python tests/gen_trap_demo.py build/trap_demo.elf
"""

import struct
import sys

from gen_demo_elf import Assembler, write_elf, BASE

# CSR 编号
MSTATUS = 0x300
MIE     = 0x304
MTVEC   = 0x305
MEPC    = 0x341
MCAUSE  = 0x342

# MMIO
UART          = 0x20000000
CLINT_MTIME   = 0x0200BFF8
CLINT_MTIMECMP = 0x02004000

MSTATUS_MIE  = 1 << 3
MIE_MTIE     = 1 << 7
MCAUSE_ECALL = 11


def build():
    asm = Assembler()

    # ---------------- 入口 ----------------
    asm.label("_start")
    asm.emit("li32", "t0", "handler")
    asm.emit("csrw", MTVEC, "t0")

    asm.emit("li", "s0", 0)          # 定时器中断计数
    asm.emit("li", "s2", 0)          # 已执行的 ecall 次数
    asm.emit("li", "s3", 3)          # 目标 ecall 次数

    asm.label("ecall_loop")
    asm.emit("ecall")                # → handler: 打印 '.', mepc += 4
    asm.emit("addi", "s2", "s2", 1)
    asm.emit("blt", "s2", "s3", "ecall_loop")

    # ---------------- 启动定时器 ----------------
    asm.emit("li32", "t0", CLINT_MTIME)
    asm.emit("lw", "t1", "t0", 0)
    asm.emit("addi", "t1", "t1", 20)          # 20 条指令之后到期
    asm.emit("li32", "t0", CLINT_MTIMECMP)
    asm.emit("sw", "t1", "t0", 0)

    asm.emit("li", "t0", MIE_MTIE)
    asm.emit("csrs", MIE, "t0")                # 打开定时器中断源
    asm.emit("li", "t0", MSTATUS_MIE)
    asm.emit("csrs", MSTATUS, "t0")            # 打开全局中断

    asm.emit("li", "s4", 3)                    # 期望 3 次 tick
    asm.label("spin")
    asm.emit("blt", "s0", "s4", "spin")

    # ---------------- 收尾 ----------------
    asm.emit("li", "t0", MSTATUS_MIE)
    asm.emit("csrci", MSTATUS, MSTATUS_MIE)    # 关中断
    asm.emit("mv", "t0", "zero")
    asm.emit("csrw", MTVEC, "t0")              # mtvec = 0 → ecall 回到宿主

    asm.emit("li", "a0", 0)
    asm.emit("li32", "a7", 93)
    asm.emit("ecall")                          # exit(0)

    # ---------------- trap handler ----------------
    asm.label("handler")
    asm.emit("csrr", "t0", MCAUSE)
    asm.emit("li", "t1", MCAUSE_ECALL)
    asm.emit("beq", "t0", "t1", "handler_ecall")

    # 定时器中断: 计数器 +1, 打印 'T', 把下一次到期时间往后推
    asm.emit("addi", "s0", "s0", 1)
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('T'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("li32", "t0", CLINT_MTIME)
    asm.emit("lw", "t2", "t0", 0)
    asm.emit("addi", "t2", "t2", 10)
    asm.emit("li32", "t0", CLINT_MTIMECMP)
    asm.emit("sw", "t2", "t0", 0)
    asm.emit("mret")

    # ecall: 打印 '.', 跳过这条 ecall 再返回
    asm.label("handler_ecall")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('.'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("csrr", "t0", MEPC)
    asm.emit("addi", "t0", "t0", 4)      # ecall 的 mepc 指向它自己
    asm.emit("csrw", MEPC, "t0")
    asm.emit("mret")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "trap_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])
    print("[gen] labels:", {k: hex(v) for k, v in labels.items()})


if __name__ == "__main__":
    main()
