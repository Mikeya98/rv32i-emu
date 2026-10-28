#!/usr/bin/env python3
"""
生成一个演示 Sv32 分页 + U 态的 RV32IM ELF。

程序流程:
    1. M 态: 在物理内存 0x80000 建两级页表
         VA 0x00010000 → PA 0x00010000  (代码页, R|X, 4KB 页)
         VA 0x40000000 → PA 0x00083000  (数据页, R|W, 4KB 页)
         VA 0x40001000 → PA 0x00084000  (只读页, R)
         VA 0xC0000000 → PA 0x00000000  (4MB 大页)
         VA 0x50000000                  (不映射)
    2. satp = Sv32 | 根表 PPN, mstatus.MPP = U, mret 进 U 态
    3. U 态依次:
         W  往 0x40000000 写数再读回 → 通过 ecall 交给 M 态打印 'W'
         S  从 4MB 大页 0xC0010000 读一个字 → 'S'
         I  执行 csrr（特权指令）→ 非法指令陷入, handler 打印 'I'
         P  往只读页 0x40001000 写 → store page fault, 打印 'P'
         F  读未映射的 0x50000000 → load page fault, 打印 'F', 退出
    4. handler 最后把特权级改回 M 并跳到 exit_stub, 用 ecall 退出

预期输出:
    WSIPF

用法:
    python tests/gen_sv32_demo.py build/sv32_demo.elf
"""

import struct
import sys

from gen_demo_elf import Assembler, write_elf, BASE

# CSR
MSTATUS = 0x300
MTVEC   = 0x305
MEPC    = 0x341
MCAUSE  = 0x342
MTVAL   = 0x343
SATP    = 0x180

UART = 0x20000000

# 物理地址
ROOT    = 0x80000
L2_CODE = 0x81000
L2_DATA = 0x82000
DATA_PA = 0x83000
RO_PA   = 0x84000

# 虚拟地址
VA_CODE  = 0x00010000
VA_DATA  = 0x40000000
VA_RO    = 0x40001000
VA_BIG   = 0xC0000000
VA_NOMAP = 0x50000000

# PTE 位
V = 1 << 0
R = 1 << 1
W = 1 << 2
X = 1 << 3
U = 1 << 4
A = 1 << 6
D = 1 << 7


def pte(pa, flags):
    return ((pa >> 12) << 10) | flags


def build():
    asm = Assembler()

    # ==================== M 态: 建页表 ====================
    asm.label("_start")

    # root[0] = 指向代码区二级表
    asm.emit("li32", "t0", pte(L2_CODE, V))
    asm.emit("li32", "t1", ROOT + 0 * 4)
    asm.emit("sw", "t0", "t1", 0)

    # root[0x100] = 指向数据区二级表
    asm.emit("li32", "t0", pte(L2_DATA, V))
    asm.emit("li32", "t1", ROOT + 0x100 * 4)
    asm.emit("sw", "t0", "t1", 0)

    # root[0x300] = 4MB 大页 → PA 0
    asm.emit("li32", "t0", pte(0, V | R | W | X | U))
    asm.emit("li32", "t1", ROOT + 0x300 * 4)
    asm.emit("sw", "t0", "t1", 0)

    # L2_CODE[0x10] = 代码页 (identity, R|X|U)
    asm.emit("li32", "t0", pte(VA_CODE, V | R | X | U))
    asm.emit("li32", "t1", L2_CODE + 0x10 * 4)
    asm.emit("sw", "t0", "t1", 0)

    # L2_DATA[0] = 可写数据页
    asm.emit("li32", "t0", pte(DATA_PA, V | R | W | U))
    asm.emit("li32", "t1", L2_DATA + 0 * 4)
    asm.emit("sw", "t0", "t1", 0)

    # L2_DATA[1] = 只读页 (没有 W)
    asm.emit("li32", "t0", pte(RO_PA, V | R | U))
    asm.emit("li32", "t1", L2_DATA + 1 * 4)
    asm.emit("sw", "t0", "t1", 0)

    # ==================== 打开 Sv32, 进 U 态 ====================
    asm.emit("li32", "t0", 0x80000000 | (ROOT >> 12))   # satp = MODE=1 | PPN
    asm.emit("csrw", SATP, "t0")

    asm.emit("li32", "t0", "user_start")
    asm.emit("csrw", MEPC, "t0")            # mret 的目标地址

    asm.emit("li32", "t0", "handler")
    asm.emit("csrw", MTVEC, "t0")           # M 态 trap handler

    asm.emit("csrw", MSTATUS, "zero")       # MPP = 00 (U), MIE = 0
    asm.emit("mret")                        # → U 态

    # ==================== U 态代码 ====================
    asm.label("user_start")

    # ---- W: 数据页读写 ----
    asm.emit("li32", "t0", VA_DATA)
    asm.emit("li32", "t1", 0x12345678)
    asm.emit("sw", "t1", "t0", 0)
    asm.emit("lw", "t2", "t0", 0)
    asm.emit("li32", "t1", 0x12345678)
    asm.emit("beq", "t2", "t1", "w_ok")
    asm.emit("li", "a0", ord('w'))          # 失败: 小写
    asm.emit("ecall")
    asm.label("w_ok")
    asm.emit("li", "a0", ord('W'))
    asm.emit("ecall")

    # ---- S: 4MB 大页读 ----
    asm.emit("li32", "t0", VA_BIG + 0x10000)    # → PA 0x10000 (本页代码)
    asm.emit("lw", "t1", "t0", 0)
    asm.emit("bnez", "t1", "s_ok")
    asm.emit("li", "a0", ord('s'))
    asm.emit("ecall")
    asm.label("s_ok")
    asm.emit("li", "a0", ord('S'))
    asm.emit("ecall")

    # ---- I: U 态执行特权指令 ----
    asm.emit("csrr", "t0", MSTATUS)         # → illegal instruction (cause 2)

    # ---- P: 往只读页写 ----
    asm.emit("li32", "t0", VA_RO)
    asm.emit("sw", "zero", "t0", 0)         # → store page fault (cause 15)

    # ---- F: 读未映射地址 ----
    asm.emit("li32", "t0", VA_NOMAP)
    asm.emit("lw", "t1", "t0", 0)           # → load page fault (cause 13)

    # 正常情况下走不到这里
    asm.emit("li", "a0", ord('!'))
    asm.emit("ecall")

    # ==================== M 态 trap handler ====================
    asm.label("handler")
    asm.emit("csrr", "t0", MCAUSE)
    asm.emit("li", "t1", 8)                 # ecall from U
    asm.emit("beq", "t0", "t1", "h_ecall")
    asm.emit("li", "t1", 2)                 # illegal instruction
    asm.emit("beq", "t0", "t1", "h_illegal")
    asm.emit("li", "t1", 15)                # store page fault
    asm.emit("beq", "t0", "t1", "h_store")
    asm.emit("li", "t1", 13)                # load page fault
    asm.emit("beq", "t0", "t1", "h_load")

    # 未知 trap: 打印 '?'
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('?'))
    asm.emit("sb", "t1", "t0", 0)
    asm.emit("j", "h_exit")

    # ---- ecall from U: a0 里是 U 态准备好的字符 ----
    asm.label("h_ecall")
    asm.emit("li32", "t0", UART)
    asm.emit("sb", "a0", "t0", 0)
    asm.emit("csrr", "t0", MEPC)
    asm.emit("addi", "t0", "t0", 4)
    asm.emit("csrw", MEPC, "t0")
    asm.emit("mret")

    # ---- 非法指令: 打印 'I', 跳过这条指令 ----
    asm.label("h_illegal")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('I'))
    asm.emit("sb", "t1", "t0", 0)
    asm.emit("csrr", "t0", MEPC)
    asm.emit("addi", "t0", "t0", 4)
    asm.emit("csrw", MEPC, "t0")
    asm.emit("mret")

    # ---- store page fault: 校验 mtval, 打印 'P' ----
    asm.label("h_store")
    asm.emit("csrr", "t0", MTVAL)
    asm.emit("li32", "t1", VA_RO)
    asm.emit("beq", "t0", "t1", "h_store_ok")
    asm.emit("j", "h_bad")
    asm.label("h_store_ok")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('P'))
    asm.emit("sb", "t1", "t0", 0)
    asm.emit("csrr", "t0", MEPC)
    asm.emit("addi", "t0", "t0", 4)
    asm.emit("csrw", MEPC, "t0")
    asm.emit("mret")

    # ---- load page fault: 校验 mtval, 打印 'F' 后退出 ----
    asm.label("h_load")
    asm.emit("csrr", "t0", MTVAL)
    asm.emit("li32", "t1", VA_NOMAP)
    asm.emit("beq", "t0", "t1", "h_load_ok")
    asm.emit("j", "h_bad")
    asm.label("h_load_ok")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('F'))
    asm.emit("sb", "t1", "t0", 0)
    asm.emit("j", "h_exit")

    # mtval 对不上 → 'x'
    asm.label("h_bad")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('x'))
    asm.emit("sb", "t1", "t0", 0)

    # ---- 收尾: 回到 M 态, 用宿主 ecall 退出 ----
    asm.label("h_exit")
    asm.emit("li32", "t0", UART)
    asm.emit("li", "t1", ord('\n'))
    asm.emit("sb", "t1", "t0", 0)

    asm.emit("li", "t0", 0)
    asm.emit("csrw", MTVEC, "t0")           # mtvec = 0 → ecall 走宿主路径
    asm.emit("li32", "t0", 0x1800)          # MPP = 11 (M)
    asm.emit("csrs", MSTATUS, "t0")
    asm.emit("li32", "t0", "exit_stub")
    asm.emit("csrw", MEPC, "t0")
    asm.emit("mret")                        # → M 态 exit_stub

    asm.label("exit_stub")
    asm.emit("li", "a0", 0)
    asm.emit("li32", "a7", 93)
    asm.emit("ecall")                       # exit(0)

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "sv32_demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])
    print("[gen] labels:", {k: hex(v) for k, v in labels.items()})


if __name__ == "__main__":
    main()
