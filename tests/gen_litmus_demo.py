#!/usr/bin/env python3
"""
生成两个 litmus test ELF (v0.18)。

    sb:  store buffering
         P0:  x = 1 ; r0 = y
         P1:  y = 1 ; r1 = x
         顺序一致下 r0 = r1 = 1；写缓冲模型下两个 load 都发生在
         store 提交之前，r0 = r1 = 0。

    mp:  message passing
         P0:  x = 1 ; [fence] ; y = 1
         P1:  等 y == 1 ; r = x
         有 fence 时 r 一定是 1。把 fence 去掉，x 和 y 落在不同的
         内存银行，y 先提交、x 后提交，P1 就会读到 y=1, x=0。

用法:
    python tests/gen_litmus_demo.py sb build/sb.elf
    python tests/gen_litmus_demo.py mp build/mp.elf
    python tests/gen_litmus_demo.py mp-nofence build/mp_nofence.elf

    build/rvemu --harts 2 --mem-model sc   build/sb.elf   → 11
    build/rvemu --harts 2 --mem-model weak build/sb.elf   → 00
    build/rvemu --harts 2 --mem-model weak build/mp.elf   → 11
    build/rvemu --harts 2 --mem-model weak build/mp_nofence.elf → 10

关键点:
  - 两个 hart 从同一入口启动，用 mhartid 分流
  - 轮转执行，每个 hart 每轮一条指令，交错顺序是确定的
  - 写缓冲延迟 = sb_lag + (addr>>2)%16，地址选得让 x/y 落在不同银行
  - 地址固定的组合保证 demo 每次输出一样
"""

import sys

from gen_demo_elf import Assembler, write_elf, BASE

UART = 0x20000000
MISA_MHARTID = 0xF14

# 数据地址：0x14000 起，避开代码段
# 提交延迟 = sb_lag + (addr>>2)%16，这里利用银行偏斜控制可见顺序
X = 0x1403C          # skew 15，提交最慢（MP 里的 x）
Y = 0x14000          # skew 0，提交最快（MP 里的 y）
Y_SB = 0x14038       # skew 14，SB 里两个地址都要慢才能读到 0


def mhartid(asm, rd):
    asm.emit("csrr", rd, MISA_MHARTID)


def exit0(asm):
    asm.emit("li", "a7", 93)
    asm.emit("li", "a0", 0)
    asm.emit("ecall")


def print_char(asm, char_reg):
    """把 char_reg 里的 ASCII 字符写到 UART"""
    asm.emit("li32", "t6", UART)
    asm.emit("sb", char_reg, "t6", 0)


def build_sb():
    asm = Assembler()
    asm.label("_start")
    mhartid(asm, "t0")
    asm.emit("bnez", "t0", "p1")

    # ---- P0: x = 1; r0 = y ----
    asm.emit("li", "t1", 1)
    asm.emit("li32", "t2", X)
    asm.emit("sw", "t1", "t2", 0)
    asm.emit("li32", "t3", Y_SB)
    asm.emit("lw", "t4", "t3", 0)
    asm.emit("addi", "t4", "t4", 48)        # '0' + r0
    print_char(asm, "t4")
    exit0(asm)

    # ---- P1: y = 1; r1 = x ----
    asm.label("p1")
    asm.emit("li", "t1", 1)
    asm.emit("li32", "t2", Y_SB)
    asm.emit("sw", "t1", "t2", 0)
    asm.emit("li32", "t3", X)
    asm.emit("lw", "t4", "t3", 0)
    asm.emit("addi", "t4", "t4", 48)
    print_char(asm, "t4")
    exit0(asm)

    code, labels = asm.assemble(BASE)
    return code, labels["_start"]


def build_mp(use_fence):
    asm = Assembler()
    asm.label("_start")
    mhartid(asm, "t0")
    asm.emit("bnez", "t0", "p1")

    # ---- P0: x = 1; [fence]; y = 1; 然后用 nop 给 P1 留时间 ----
    asm.emit("li", "t1", 1)
    asm.emit("li32", "t2", X)
    asm.emit("sw", "t1", "t2", 0)
    asm.emit("fence" if use_fence else "nop")
    asm.emit("li32", "t3", Y)
    asm.emit("sw", "t1", "t3", 0)
    for _ in range(12):
        asm.emit("nop")
    exit0(asm)

    # ---- P1: 自旋等 y == 1, 然后读 x ----
    asm.label("p1")
    asm.emit("li32", "s0", Y)               # 自旋循环只留 3 条指令
    asm.label("wait")
    asm.emit("lw", "t1", "s0", 0)
    asm.emit("bnez", "t1", "got")
    asm.emit("j", "wait")

    asm.label("got")
    asm.emit("li32", "s1", X)
    asm.emit("lw", "t2", "s1", 0)
    asm.emit("addi", "t1", "t1", 48)        # y = 1 → '1'
    print_char(asm, "t1")
    asm.emit("addi", "t2", "t2", 48)        # x → '0' / '1'
    print_char(asm, "t2")
    asm.emit("li", "a0", 10)
    print_char(asm, "a0")
    exit0(asm)

    code, labels = asm.assemble(BASE)
    return code, labels["_start"]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    kind, out = sys.argv[1], sys.argv[2]
    if kind == "sb":
        code, entry = build_sb()
    elif kind == "mp":
        code, entry = build_mp(True)
    elif kind == "mp-nofence":
        code, entry = build_mp(False)
    else:
        print(f"unknown kind: {kind}")
        return 1

    write_elf(out, code, entry)
    return 0


if __name__ == "__main__":
    sys.exit(main())
