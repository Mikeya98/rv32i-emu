#!/usr/bin/env python3
"""
生成一个可在 rvemu 里运行的 RV32IM ELF 文件。

机器上没有 riscv-gnu-toolchain 也没关系：这里用 Python 手写一个小汇编器，
把几条指令编码成机器码，再拼一个最小的 ELF32 文件头。

用法:
    python tests/gen_demo_elf.py build/demo.elf

程序做的事:
    1. 打印 "10! = "
    2. 用 MUL 循环算 10 的阶乘
    3. 用 DIVU/REMU 把结果转成十进制数字（倒着写进缓冲区）
    4. 用 ecall write 输出，最后 ecall exit(0)
"""

import struct
import sys

BASE = 0x10000          # 代码链接地址 (.text)
DATA = 0x11000          # 数据段地址 (只占 memsz, 用于 BSS)
DATA_SIZE = 0x100

REGS = {
    "zero": 0, "ra": 1, "sp": 2, "gp": 3, "tp": 4,
    "t0": 5, "t1": 6, "t2": 7, "s0": 8, "s1": 9,
    "a0": 10, "a1": 11, "a2": 12, "a3": 13, "a4": 14, "a5": 15,
    "a6": 16, "a7": 17,
    "s2": 18, "s3": 19, "s4": 20, "s5": 21, "s6": 22, "s7": 23,
    "s8": 24, "s9": 25, "s10": 26, "s11": 27,
    "t3": 28, "t4": 29, "t5": 30, "t6": 31,
}


def reg(x):
    return REGS[x] if isinstance(x, str) else int(x)


# =========================== 指令编码 ===========================

def r_type(f7, rs2, rs1, f3, rd, op=0x33):
    return ((f7 & 0x7F) << 25) | ((reg(rs2) & 0x1F) << 20) | \
           ((reg(rs1) & 0x1F) << 15) | ((f3 & 7) << 12) | \
           ((reg(rd) & 0x1F) << 7) | (op & 0x7F)


def i_type(imm, rs1, f3, rd, op=0x13):
    return ((imm & 0xFFF) << 20) | ((reg(rs1) & 0x1F) << 15) | \
           ((f3 & 7) << 12) | ((reg(rd) & 0x1F) << 7) | (op & 0x7F)


def s_type(imm, rs2, rs1, f3, op=0x23):
    return (((imm >> 5) & 0x7F) << 25) | ((reg(rs2) & 0x1F) << 20) | \
           ((reg(rs1) & 0x1F) << 15) | ((f3 & 7) << 12) | \
           ((imm & 0x1F) << 7) | (op & 0x7F)


def b_type(imm, rs2, rs1, f3, op=0x63):
    return (((imm >> 12) & 1) << 31) | (((imm >> 5) & 0x3F) << 25) | \
           ((reg(rs2) & 0x1F) << 20) | ((reg(rs1) & 0x1F) << 15) | \
           ((f3 & 7) << 12) | (((imm >> 1) & 0xF) << 8) | \
           (((imm >> 11) & 1) << 7) | (op & 0x7F)


def u_type(imm20, rd, op):
    return ((imm20 & 0xFFFFF) << 12) | ((reg(rd) & 0x1F) << 7) | (op & 0x7F)


# =========================== 小汇编器 ===========================

class Assembler:
    def __init__(self):
        self.items = []

    def label(self, name):
        self.items.append(("label", name))

    def data(self, raw):
        self.items.append(("data", raw))

    def emit(self, mnemonic, *args):
        self.items.append(("insn", mnemonic, args))

    # 每条指令占多少个 32-bit 字
    def _words(self, item):
        if item[0] != "insn":
            return 0
        return 2 if item[1] == "li32" else 1

    def assemble(self, base=BASE):
        # ---- pass 1: 计算标签地址 ----
        labels = {}
        addr = base
        for item in self.items:
            if item[0] == "label":
                labels[item[1]] = addr
            elif item[0] == "data":
                addr += len(item[1])
            else:
                addr += 4 * self._words(item)

        # ---- pass 2: 编码 ----
        out = bytearray()
        addr = base

        def value(x):
            if isinstance(x, str):
                return labels[x]
            return x

        for item in self.items:
            if item[0] == "label":
                continue
            if item[0] == "data":
                out += item[1]
                addr += len(item[1])
                continue

            _, mn, args = item

            if mn == "li32":
                rd, val = args
                val = value(val) & 0xFFFFFFFF
                hi = (val + 0x800) >> 12
                lo = val - (hi << 12)
                out += struct.pack("<I", u_type(hi, rd, 0x37))
                out += struct.pack("<I", i_type(lo, rd, 0, rd, 0x13))
                addr += 8

            elif mn == "mv":
                rd, rs = args
                out += struct.pack("<I", i_type(0, rs, 0, rd))
                addr += 4

            elif mn == "addi":
                rd, rs, imm = args
                out += struct.pack("<I", i_type(imm, rs, 0, rd))
                addr += 4

            elif mn == "add":
                rd, rs1, rs2 = args
                out += struct.pack("<I", r_type(0, rs2, rs1, 0, rd))
                addr += 4

            elif mn == "sub":
                rd, rs1, rs2 = args
                out += struct.pack("<I", r_type(0x20, rs2, rs1, 0, rd))
                addr += 4

            elif mn == "mul":
                rd, rs1, rs2 = args
                out += struct.pack("<I", r_type(0x01, rs2, rs1, 0, rd))
                addr += 4

            elif mn == "divu":
                rd, rs1, rs2 = args
                out += struct.pack("<I", r_type(0x01, rs2, rs1, 5, rd))
                addr += 4

            elif mn == "remu":
                rd, rs1, rs2 = args
                out += struct.pack("<I", r_type(0x01, rs2, rs1, 7, rd))
                addr += 4

            elif mn == "sb":
                rs2, rs1, imm = args
                out += struct.pack("<I", s_type(imm, rs2, rs1, 0))
                addr += 4

            elif mn == "sw":
                rs2, rs1, imm = args
                out += struct.pack("<I", s_type(imm, rs2, rs1, 2))
                addr += 4

            elif mn == "lw":
                rd, rs1, imm = args
                out += struct.pack("<I", i_type(imm, rs1, 2, rd, 0x03))
                addr += 4

            elif mn == "li":
                rd, imm = args
                out += struct.pack("<I", i_type(imm, 0, 0, rd, 0x13))
                addr += 4

            elif mn == "bne":
                rs1, rs2, label = args
                off = labels[label] - addr
                out += struct.pack("<I", b_type(off, rs2, rs1, 1))
                addr += 4

            elif mn == "beq":
                rs1, rs2, label = args
                off = labels[label] - addr
                out += struct.pack("<I", b_type(off, rs2, rs1, 0))
                addr += 4

            elif mn == "blt":
                rs1, rs2, label = args
                off = labels[label] - addr
                out += struct.pack("<I", b_type(off, rs2, rs1, 4))
                addr += 4

            elif mn == "mret":
                out += struct.pack("<I", 0x30200073)
                addr += 4

            elif mn in ("csrrw", "csrrs", "csrrc"):
                rd, csr, rs1 = args
                f3 = {"csrrw": 1, "csrrs": 2, "csrrc": 3}[mn]
                out += struct.pack("<I", i_type(csr, rs1, f3, rd, 0x73))
                addr += 4

            elif mn in ("csrrwi", "csrrsi", "csrrci"):
                rd, csr, uimm = args
                f3 = {"csrrwi": 5, "csrrsi": 6, "csrrci": 7}[mn]
                out += struct.pack("<I", i_type(csr, uimm, f3, rd, 0x73))
                addr += 4

            elif mn == "csrw":          # csrw csr, rs  → csrrw x0, csr, rs
                csr, rs1 = args
                out += struct.pack("<I", i_type(csr, rs1, 1, 0, 0x73))
                addr += 4

            elif mn == "csrr":          # csrr rd, csr  → csrrs rd, csr, x0
                rd, csr = args
                out += struct.pack("<I", i_type(csr, 0, 2, rd, 0x73))
                addr += 4

            elif mn == "csrs":          # csrs csr, rs  → csrrs x0, csr, rs
                csr, rs1 = args
                out += struct.pack("<I", i_type(csr, rs1, 2, 0, 0x73))
                addr += 4

            elif mn == "csrci":         # csrci csr, uimm → csrrci x0, csr, uimm
                csr, uimm = args
                out += struct.pack("<I", i_type(csr, uimm, 7, 0, 0x73))
                addr += 4

            elif mn == "bnez":
                rs1, label = args
                off = labels[label] - addr
                out += struct.pack("<I", b_type(off, 0, rs1, 1))
                addr += 4

            elif mn == "ecall":
                out += struct.pack("<I", 0x00000073)
                addr += 4

            else:
                raise SystemExit("unknown mnemonic: %s" % mn)

        return bytes(out), labels


# =========================== ELF32 写出 ===========================

def write_elf(path, code, entry):
    code_off = 0x1000
    data_off = 0x2000

    e_ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8   # ELF32, LE
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,          # e_type = ET_EXEC
        243,        # e_machine = EM_RISCV
        1,          # e_version
        entry,      # e_entry
        52,         # e_phoff
        0,          # e_shoff
        0,          # e_flags
        52,         # e_ehsize
        32,         # e_phentsize
        2,          # e_phnum
        40,         # e_shentsize
        0,          # e_shnum
        0,          # e_shstrndx
    )

    ph_code = struct.pack(
        "<IIIIIIII",
        1, code_off, BASE, BASE, len(code), len(code), 5, 0x1000)  # R+X
    ph_data = struct.pack(
        "<IIIIIIII",
        1, data_off, DATA, DATA, 0, DATA_SIZE, 6, 0x1000)          # RW, BSS

    buf = bytearray()
    buf += ehdr + ph_code + ph_data
    buf += b"\x00" * (code_off - len(buf))
    buf += code
    if len(buf) > data_off:
        raise SystemExit("code too large")
    buf += b"\x00" * (data_off - len(buf))

    with open(path, "wb") as f:
        f.write(buf)

    print("[gen] %s: %d bytes code, entry=0x%08X" % (path, len(code), entry))


# =========================== 程序 ===========================

def build():
    asm = Assembler()

    asm.label("_start")

    # write(1, msg, msg_len)
    asm.emit("li32", "a1", "msg")
    asm.emit("li32", "a0", 1)
    asm.emit("li32", "a2", 6)          # len("10! = ")
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    # acc = 1; i = 10; do { acc *= i; i-- } while (i)
    asm.emit("li32", "a0", 1)
    asm.emit("li32", "t0", 10)
    asm.label("fact_loop")
    asm.emit("mul", "a0", "a0", "t0")
    asm.emit("addi", "t0", "t0", -1)
    asm.emit("bnez", "t0", "fact_loop")

    # 十进制转换: 从缓冲区尾部往前写
    asm.emit("li32", "t2", DATA + 0x80)     # buf_end
    asm.emit("mv", "t3", "t2")
    asm.emit("li32", "t4", 10)
    asm.label("digit_loop")
    asm.emit("remu", "t5", "a0", "t4")
    asm.emit("divu", "a0", "a0", "t4")
    asm.emit("addi", "t5", "t5", 48)
    asm.emit("addi", "t3", "t3", -1)
    asm.emit("sb", "t5", "t3", 0)
    asm.emit("bnez", "a0", "digit_loop")

    # write(1, buf, buf_end - buf)
    asm.emit("li32", "a0", 1)
    asm.emit("mv", "a1", "t3")
    asm.emit("sub", "a2", "t2", "t3")
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    # write(1, "\n", 1)
    asm.emit("li32", "a1", "nl")
    asm.emit("li32", "a0", 1)
    asm.emit("li32", "a2", 1)
    asm.emit("li32", "a7", 64)
    asm.emit("ecall")

    # exit(0)
    asm.emit("li32", "a0", 0)
    asm.emit("li32", "a7", 93)
    asm.emit("ecall")

    asm.label("msg")
    asm.data(b"10! = ")
    asm.label("nl")
    asm.data(b"\n")

    return asm.assemble(BASE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "demo.elf"
    code, labels = build()
    write_elf(out, code, labels["_start"])
    print("[gen] labels:", {k: hex(v) for k, v in labels.items()})


if __name__ == "__main__":
    main()
