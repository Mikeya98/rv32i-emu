#!/usr/bin/env python3
"""
生成一个演示 S 态 + 异常委托的 RV32IM ELF。

程序流程:
    1. M 态: 设置 stvec、medeleg (只委托 U 态 ecall), mepc=user_start,
       mstatus.MPP=U, mret 进 U 态
    2. U 态: 执行 ecall (cause 8)
    3. medeleg[8]=1 → trap 直接进 S 态 handler, 不经过 M 态
       S handler: 往 UART 写 'S', 再把 scause 的低 4 位写成字符 ('8'),
                  sepc += 4, sret 返回 U 态
    4. U 态继续: 写 'U', 然后 ebreak 停机

预期输出:
    S8U

用法:
    python tests/gen_s_demo.py build/s_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

# CSR
SSTATUS = 0x100
STVEC   = 0x105
SEPC    = 0x141
SCAUSE  = 0x142
SATP    = 0x180
MSTATUS = 0x300
MEDELEG = 0x302
MEPC    = 0x341
MCAUSE  = 0x342

UART = 0x20000000

# 委托位: ecall from U = cause 8
DELEG_ECALL_U = 1 << 8


def build():
    asm = Assembler()
    asm.label("_start")

    # M 态: stvec = S handler
    asm.emit("li32", "t0", "s_handler")
    asm.emit("csrw", STVEC, "t0")

    # 只把 U 态 ecall 委托给 S; 其他 trap 仍归 M
    asm.emit("li32", "t0", DELEG_ECALL_U)
    asm.emit("csrw", MEDELEG, "t0")

    # satp = 0 (bare): 这个 demo 只演示特权级/委托, 不演示分页
    asm.emit("csrw", SATP, "zero")

    # 进 U 态: MPP = 00, MIE = 0
    asm.emit("li32", "t0", "user_start")
    asm.emit("csrw", MEPC, "t0")
    asm.emit("csrw", MSTATUS, "zero")
    asm.emit("mret")

    # ---------------- S 态 handler ----------------
    asm.label("s_handler")
    # 往 UART 写 'S'
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('S'))
    asm.emit("sb", "t1", "t0", 0)

    # 检查 scause 是不是 8 (ecall from U), 打印 '8' 或 '?'
    asm.emit("csrr", "t1", SCAUSE)
    asm.emit("li", "t2", 8)
    asm.emit("beq", "t1", "t2", "s_cause_ok")
    asm.emit("li", "t1", ord('?'))
    asm.emit("j", "s_put")
    asm.label("s_cause_ok")
    asm.emit("li", "t1", ord('8'))
    asm.label("s_put")
    asm.emit("sb", "t1", "t0", 0)

    # sepc += 4, sret 回到 U 态下一条指令
    asm.emit("csrr", "t1", SEPC)
    asm.emit("addi", "t1", "t1", 4)
    asm.emit("csrw", SEPC, "t1")
    asm.emit("sret")

    # ---------------- U 态代码 ----------------
    asm.label("user_start")
    asm.emit("ecall")                       # → 委托给 S handler

    # 回到这里说明 sret 正常: 写 'U' 和换行
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('U'))
    asm.emit("sb", "t1", "t0", 0)
    asm.emit("li", "t1", ord('\n'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("ebreak")                      # mtvec=0 → 停机

    # M 态兜底 handler (这个 demo 不该走到这里)
    asm.label("m_handler")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('M'))
    asm.emit("sb", "t1", "t0", 0)
    asm.emit("ebreak")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "s_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])
    print("[gen] labels:", {k: hex(v) for k, v in labels.items()})


if __name__ == "__main__":
    main()
