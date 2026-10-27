# RV32IM Emulator — 纯 C 手写 RISC-V CPU 模拟器

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![ISA](https://img.shields.io/badge/ISA-RV32IM-brightgreen.svg)](https://riscv.org/)
[![Language](https://img.shields.io/badge/language-C11-orange.svg)]()

一个从零手写的 **RISC-V RV32IM 指令集模拟器**，纯 C 实现，零依赖。

**不需要开发板，也不需要交叉编译工具链。** `make && make demo` 就能跑起来；仓库里的 `tests/gen_demo_elf.py` 用 Python 手写了一个小汇编器，直接生成可加载的 ELF。

## 特性

- **RV32I 基础整数指令集** — 40 条指令：ALU、load/store、分支跳转、LUI/AUIPC、CSR
- **RV32M 乘除法** — mul/mulh/mulhsu/mulhu/div/divu/rem/remu，含除零和溢出语义
- **ELF 加载器** — 手写 32-bit RISC-V ELF 解析，不依赖 libelf
- **1MB 扁平内存** — 字节寻址，load/store 带边界检查；`0x20000000` 写字节即输出到终端
- **系统调用** — `ecall` 按 a7 分发（exit / write），接近 RISC-V Linux ABI
- **交互式调试器** — 单步、继续、断点、寄存器、内存、反汇编
- **CSR** — mstatus/mie/mtvec/mscratch/mepc/mcause/mtval/mip/mcycle
- **trap 框架** — 异常（ecall / 非法指令 / 访存错误 / 非对齐）和中断（机器定时器）统一进入 mtvec，`mret` 返回
- **简化 CLINT** — `mtime` / `mtimecmp`（地址与真实 CLINT 一致），可做定时器中断
- **自测** — 解码 45 项 + 执行 43 项 + trap 20 项，`make test` 全过

## 快速开始

```bash
# 编译（只需 gcc + make）
git clone git@github.com:Mikeya98/rv32i-emu.git
cd rv32i-emu
make

# 自测
make test

# 跑 demo：算 10! 并打印
make demo

# trap demo：ecall 陷入 + mret 返回 + 定时器中断
make trap-demo

# 在调试器里打开同一个程序
make debug-demo

# 也可以加载自己的 ELF
./build/rvemu -d your_program.elf
```

`make demo` 的输出：

```
10! = 3628800
[SYSTEM] exit(0)
[RUN] 111 instructions executed
```

## 调试器

```
$ ./build/rvemu -d build/demo.elf
=== RV32I Debugger ===
[RUNNING] pc=0x00010000
  0x00010000: lui a1, 0x10
(rvdbg) d 0x10000 6
  0x00010000: lui a1, 0x10
  0x00010004: addi a1, a1, 192
  0x00010008: lui a0, 0x0
  0x0001000C: addi a0, a0, 1
  ...
(rvdbg) b 0x10034
breakpoint 0 set at 0x00010034
(rvdbg) c
10! = [break] hit breakpoint
[RUNNING] pc=0x00010034
  0x00010034: mul a0, a0, t0
```

| 命令 | 作用 |
|------|------|
| `s [n]` | 单步 n 条（默认 1），执行前后各打印一条反汇编 |
| `c` | 继续运行，遇到断点或停机时返回 |
| `r` | 打印 32 个寄存器和 CSR |
| `x <addr> [n]` | 按字节 dump 内存 |
| `d [addr] [n]` | 反汇编 n 条指令 |
| `b <addr>` / `bl` / `bd <addr>` / `bc` | 断点增删查清 |
| `q` | 退出 |

## 架构

```
main.c → emu.c (fetch→decode→execute)
           ├── elf.c       ELF 解析器
           ├── decode.c    指令解码 + 反汇编
           ├── execute.c   执行引擎 (RV32I + RV32M + ecall)
           ├── debug.c     交互式调试器
           ├── trap.c      trap 框架 (异常/中断/mret) + 简化 CLINT
           ├── regfile.c   寄存器文件 (x0-x31 + PC)
           ├── memory.c    1MB 内存 + MMIO UART
           └── csr.c       CSR 控制寄存器
```

## 测试程序从哪来

机器上没装 `riscv64-unknown-elf-gcc`，所以 `tests/gen_demo_elf.py` 自己做了两件事：

1. 把 `li32 / mul / divu / remu / sb / bne / ecall` 这些助记符编码成机器码；
2. 拼一个最小 ELF32 文件头（一个 RX 段 + 一个 RW 段）。

```bash
python tests/gen_demo_elf.py build/demo.elf
```

demo 程序用 `MUL` 算 10 的阶乘，再用 `DIVU`/`REMU` 把结果转成十进制数字，最后通过 `ecall write` 输出。乘除法、系统调用、分支和内存写都能在这一个程序里跑一遍。

`tests/gen_trap_demo.py` 演示 trap：装好 `mtvec` 后连续执行三次 `ecall`，handler 每次打印一个 `.`、把 `mepc` 加 4 跳过这条 `ecall` 再 `mret`；然后打开定时器中断，handler 每次打印一个 `T` 并把 `mtimecmp` 往后推。最后把 `mtvec` 清零，用 `ecall` 走宿主 syscall 退出。

```
$ make trap-demo
...TTT
[SYSTEM] exit(0)
```

## 进度

| 模块 | 状态 |
|------|:----:|
| 类型定义 + 基础框架 | ✅ |
| 内存模型 (1MB) | ✅ |
| 寄存器文件 (x0-x31) | ✅ |
| 指令解码 (6 格式) | ✅ |
| ELF 加载器 | ✅ |
| RV32I 执行引擎 (40 条) | ✅ |
| CSR (最小特权) | ✅ |
| RV32M 乘除法 | ✅ |
| ecall 系统调用 (exit/write) | ✅ |
| 交互式调试器 | ✅ |
| trap 框架 (异常 + 定时器中断 + mret) | ✅ |

## License

MIT
