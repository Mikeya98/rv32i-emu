#!/usr/bin/env python3
"""
生成一个演示 mstatus.FS (浮点单元状态) 的 ELF。

程序流程:
    1. mtvec 指向本程序的 trap handler
    2. mstatus.FS = 00 (Off), 执行 fadd.s → 非法指令, 进 handler
       handler 检查 mcause=2 / mtval=指令, 打印 'T', 把 FS 改成 Initial,
       mepc += 4 跳过故障指令
    3. 再把 FS 清成 Off, 执行 csrrs t1, fcsr, x0 → 同样非法, 打印 'C'
    4. FS=Initial 时执行一条 fadd.s → 正常执行, FS 自动变成 Dirty
    5. 读 mstatus 检查 FS==11, 打印 'F'
    6. mtvec 清零, ecall exit(0)

预期输出:
    TCF

用法:
    python tests/gen_fs_demo.py build/fs_demo.elf
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

UART = 0x20000000

CSR_MSTATUS = 0x300
CSR_MTVEC   = 0x305
CSR_MEPC    = 0x341
CSR_MCAUSE  = 0x342
CSR_MTVAL   = 0x343
CSR_FCSR    = 0x003

FS_MASK     = 0x6000        # mstatus.FS[14:13]
FS_INITIAL  = 0x2000

# fadd.s f1, f2, f3
FADD = (0x00 << 25) | (3 << 20) | (2 << 15) | (0 << 12) | (1 << 7) | 0x53
# csrrs t1, fcsr, x0  (读 fcsr)
CSR_READ_FCSR = (CSR_FCSR << 20) | (0 << 15) | (2 << 12) | (6 << 7) | 0x73


def build():
    asm = Assembler()
    asm.label("_start")

    asm.emit("li32", "s2", UART)            # s2 = UART 地址
    asm.emit("li32", "t0", "handler")
    asm.emit("csrw", CSR_MTVEC, "t0")

    # ---- 1. FS=Off, FP 指令非法 ----
    asm.emit("li32", "t0", FS_MASK)
    asm.emit("csrrc", "zero", CSR_MSTATUS, "t0")
    asm.emit("word", FADD)                  # → trap, handler 返回后已跳过

    # ---- 2. FS=Off, 读 fcsr 非法 ----
    asm.emit("li32", "t0", FS_MASK)
    asm.emit("csrrc", "zero", CSR_MSTATUS, "t0")
    asm.emit("word", CSR_READ_FCSR)         # → trap, 打印 'C'

    # ---- 3. FS=Initial (handler 已恢复), FP 指令正常执行 ----
    asm.emit("word", FADD)                  # 执行成功, FS → Dirty

    # ---- 4. 检查 FS == Dirty ----
    asm.emit("csrr", "t0", CSR_MSTATUS)
    asm.emit("li32", "t1", FS_MASK)
    asm.emit("blt", "t0", "t1", "fail")
    asm.emit("li", "s1", ord('F'))
    asm.emit("sb", "s1", "s2", 0)
    asm.emit("j", "done")

    asm.label("fail")
    asm.emit("li", "s1", ord('X'))
    asm.emit("sb", "s1", "s2", 0)

    asm.label("done")
    asm.emit("li", "s1", 10)
    asm.emit("sb", "s1", "s2", 0)
    asm.emit("csrw", CSR_MTVEC, "zero")     # 关掉 handler, 让 ecall 走宿主
    asm.emit("li", "a7", 93)
    asm.emit("li", "a0", 0)
    asm.emit("ecall")

    # =================== trap handler ===================
    asm.label("handler")
    asm.emit("csrr", "t0", CSR_MCAUSE)
    asm.emit("li", "t1", 2)
    asm.emit("bne", "t0", "t1", "bad")

    asm.emit("csrr", "t0", CSR_MTVAL)
    asm.emit("li32", "t1", FADD)
    asm.emit("beq", "t0", "t1", "trap_fpu")
    asm.emit("li32", "t1", CSR_READ_FCSR)
    asm.emit("beq", "t0", "t1", "trap_csr")
    asm.emit("j", "bad")

    asm.label("trap_fpu")
    asm.emit("li", "s1", ord('T'))
    asm.emit("sb", "s1", "s2", 0)
    asm.emit("j", "enable_fs")

    asm.label("trap_csr")
    asm.emit("li", "s1", ord('C'))
    asm.emit("sb", "s1", "s2", 0)

    asm.label("enable_fs")
    asm.emit("li32", "t2", FS_INITIAL)
    asm.emit("csrs", CSR_MSTATUS, "t2")
    asm.emit("j", "skip")

    asm.label("bad")
    asm.emit("li", "s1", ord('X'))
    asm.emit("sb", "s1", "s2", 0)

    asm.label("skip")
    asm.emit("csrr", "t3", CSR_MEPC)
    asm.emit("addi", "t3", "t3", 4)
    asm.emit("csrw", CSR_MEPC, "t3")
    asm.emit("mret")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/fs_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])


if __name__ == "__main__":
    main()
