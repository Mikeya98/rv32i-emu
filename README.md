# RV32I Emulator — 纯 C 手写 RISC-V CPU 模拟器

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![ISA](https://img.shields.io/badge/ISA-RV32I-brightgreen.svg)](https://riscv.org/)
[![Language](https://img.shields.io/badge/language-C11-orange.svg)]()

一个从零手写的 **RISC-V RV32I 指令集模拟器**，纯 C 实现，零依赖。

**不需要开发板，不需要交叉编译工具链，clone 下来 `make && ./rvemu` 就能跑。**

## 特性

- ✅ **RV32I 基础整数指令集** — 40 条指令完整支持（开发中）
- ✅ **ELF 加载器** — 手写 32-bit RISC-V ELF 解析，不依赖 libelf
- ✅ **64KB 虚拟内存** — 字节寻址，load/store 带边界检查
- ✅ **寄存器文件** — x0-x31 + PC，x0 硬连线为 0
- ✅ **指令解码器** — R/I/S/B/U/J 六种格式完整解析
- ✅ **CSR 支持** — mstatus/mtvec/mepc/mcause/mcycle
- ⏳ **执行引擎** — 开发中

## 快速开始

```bash
# 编译（只需 gcc + make）
git clone git@github.com:Mikeya98/rv32i-emu.git
cd rv32i-emu
make

# 加载 RISC-V ELF 文件
./build/rvemu tests/hello.elf

# 输出示例：
# ┌──────────────────────────────────────┐
# │  RV32I Emulator v0.1                 │
# │  RISC-V RV32I Base Integer ISA       │
# └──────────────────────────────────────┘
# [ELF] === Header ===
#   type      = 2 (ET_EXEC)
#   machine   = 243 (EM_RISCV)
#   entry     = 0x00010074
#   phnum     = 3
# [ELF] === Program Headers ===
#   Type   Offset     VirtAddr   FileSiz    MemSiz     Flags
#   1      0x00001000 0x00010000 0x000000BC 0x000000BC R-X
#           → loaded 188 bytes to [0x00010000 — 0x000100BB]
#   1      0x00002000 0x00011000 0x00000010 0x00000018 RW-
#           → loaded 16 bytes to [0x00011000 — 0x0001100F]
#           → bss zeroed [0x00011010 — 0x00011017] (8 bytes)
# [ELF] Load OK. entry=0x00010074, 2 PT_LOAD segments loaded.
```

## 架构

```
main.c → emu.c (fetch→decode→execute)
           ├── elf.c       ELF 解析器
           ├── decode.c    指令解码 (R/I/S/B/U/J)
           ├── execute.c   执行引擎 (开发中)
           ├── regfile.c   寄存器文件 (x0-x31 + PC)
           ├── memory.c    内存模型 (64KB + MMIO UART)
           └── csr.c       CSR 控制寄存器
```

## 为什么写这个？

RISC-V 是当前最热门的开源指令集架构。理解 CPU 如何执行指令，最快的方法不是看书——是写一个。

这个项目是[知乎专栏「从零构建 RISC-V 模拟器」](https://www.zhihu.com/column/riscv-emu)的配套代码。系列文章逐步讲解从 ELF 解析到指令执行的完整过程。

## 进度

| 模块 | 状态 |
|------|:----:|
| 类型定义 + 基础框架 | ✅ |
| 内存模型 (64KB) | ✅ |
| 寄存器文件 (x0-x31) | ✅ |
| 指令解码 (6 格式) | ✅ |
| ELF 加载器 | ✅ |
| CSR (最小特权) | ✅ |
| 执行引擎 (40 指令) | 🔜 |
| 主循环 + 单步调试 | 🔜 |

## License

MIT
