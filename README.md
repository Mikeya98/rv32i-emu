# RV32IMAFC(D) Emulator — 纯 C 手写 RISC-V CPU 模拟器

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![ISA](https://img.shields.io/badge/ISA-RV32IMAFC%2BD-brightgreen.svg)](https://riscv.org/)
[![Language](https://img.shields.io/badge/language-C11-orange.svg)]()

一个从零手写的 **RISC-V RV32IMAFC(D) 指令集模拟器**，纯 C 实现，零依赖。

**不需要开发板，也不需要交叉编译工具链。** `make && make demo` 就能跑起来；仓库里的 `tests/gen_demo_elf.py` 用 Python 手写了一个小汇编器，直接生成可加载的 ELF。

## 特性

- **RV32I 基础整数指令集** — 40 条指令：ALU、load/store、分支跳转、LUI/AUIPC、CSR
- **RV32M 乘除法** — mul/mulh/mulhsu/mulhu/div/divu/rem/remu，含除零和溢出语义
- **RV32A 原子指令** — lr.w / sc.w / amoswap / amoadd / amoxor / amoand / amoor / amomin / amomax / amominu / amomaxu；单 hart 用 (valid, addr) 保留集实现 LR/SC，任何命中地址的写都会让 SC 失败
- **融合乘加 FMA** — fmadd/fmsub/fnmsub/fnmadd 的 `.s` 和 `.d` 两种格式；四种形式化成 `fma(a', b, c')`，宿主 `fmaf/fma` 保证只舍入一次。符号变换作用在输入端而不是结果端，避免 RDN/RUP 下"舍入后取反"出错
- **RV32D 双精度浮点** — fadd/fsub/fmul/fdiv/fsqrt、fsgnj/fsgnjn/fsgnjx、fmin/fmax、feq/flt/fle、fcvt.w.d/fcvt.wu.d/fcvt.d.w/fcvt.d.wu、fcvt.s.d/fcvt.d.s、fclass.d、fld/fsd；FLEN 从 32 扩到 64，单精度值按 NaN-box 规则存高半字全 1
- **RV32F 单精度浮点** — fadd/fsub/fmul/fdiv/fsqrt、fsgnj/fsgnjn/fsgnjx、fmin/fmax、feq/flt/fle、fcvt.w.s/fcvt.wu.s/fcvt.s.w/fcvt.s.wu、fmv.x.w/fmv.w.x/fclass、flw/fsw；fflags/frm/fcsr 三个 CSR，舍入和异常标志走宿主 fenv
- **mstatus.FS 浮点状态** — FS[14:13] 的 Off/Initial/Clean/Dirty 四态：Off 时任何 FP 指令（含 flw/fsw/fld/fsd）和 fflags/frm/fcsr 访问都按非法指令陷入；FS 非零时执行 FP 指令或写浮点 CSR 自动置 Dirty；sstatus 里能看到同一组位，S 态软件也能开关浮点
- **RV32C 压缩指令** — 16 位指令先展开成等价的 32 位指令，再复用 RV32I 的执行路径；覆盖 c.addi/c.li/c.lui/c.addi4spn/c.addi16sp、c.lw/c.sw/c.lwsp/c.swsp、c.slli/c.srli/c.srai/c.andi/c.sub/c.xor/c.or/c.and、c.beqz/c.bnez/c.j/c.jal/c.jr/c.jalr/c.mv/c.add/c.ebreak；取指按 2 字节对齐，`jal/jalr` 的链接值也是 pc+2
- **ELF 加载器** — 手写 32-bit RISC-V ELF 解析，不依赖 libelf
- **1MB 扁平内存** — 字节寻址，load/store 带边界检查；`0x20000000` 写字节即输出到终端
- **系统调用** — `ecall` 按 a7 分发（exit / write），接近 RISC-V Linux ABI
- **交互式调试器** — 单步、继续、断点、寄存器、内存、反汇编；能识别 16 位压缩指令，反汇编时标 `[c]` 并按 2 字节步进
- **CSR** — mstatus/mie/mtvec/mscratch/mepc/mcause/mtval/mip/mcycle/fcsr/fflags/frm
- **trap 框架** — 异常（ecall / 非法指令 / 访存错误 / 非对齐）和中断（机器定时器）统一进入 mtvec，`mret` 返回
- **Sv32 分页** — 两级页表（4KB 页 + 4MB 大页）、satp、PTE 权限位（V/R/W/X/U/A/D）、取指/读/写三类缺页（mcause 12/13/15）
- **U 态** — M/U 两级特权，MPP/MPIE 保存与恢复，`mret` 回 U 态；U 态访问 M 态 CSR、执行 `mret`/`sfence.vma` 都按非法指令陷入
- **S 态与异常委托** — S 态 CSR（sstatus/sie/stvec/sscratch/sepc/scause/stval/sip）、`sret`、`medeleg`/`mideleg`；当前级不高于 S 且对应委托位为 1 的 trap 直接进 `stvec`，否则进 `mtvec`；S 态默认不能访问 U 页，打开 `mstatus.SUM` 后可以读写但仍不能取指
- **多 hart（v0.12）** — 最多 4 个核，每个 hart 有自己的寄存器、CSR、特权级、LR/SC 保留集和 `mtimecmp`，共享同一块物理内存；所有 hart 从同一入口启动，用 `mhartid` 分流（和真机固件的做法一致）；`emu_run` 按轮转顺序交错执行，结果确定可复现
- **跨核原子语义** — 任何一个 hart 的普通 store / AMO / SC 写到某地址，都会让所有 hart 里针对该地址的 LR 保留失效，双核下的 `lr/sc` 不丢更新
- **核间中断（v0.13）** — CLINT 的 `msip` 每个 hart 一个（`0x02000000 + 4*hartid`），写 1 拉起目标核的软件中断，写 0 撤掉；`mie.MSIE` / `mip.MSIP` / `mcause = 0x80000003` 全部接通。电平语义，handler 里不清就一直挂；同时 pending 时软件中断优先于定时器中断
- **PLIC 外部中断（v0.14）** — 32 个中断源、每个 hart 一个上下文；优先级寄存器、pending、enable、threshold、claim/complete 全按 QEMU virt 的地址布局实现。设备写门铃寄存器拉起 pending，`mip.MEIP` 是"线"语义，claim 清掉 pending 后自动落下；`mie.MEIE` / `mcause = 0x8000000B` 接通，优先级按规范排序（外部 > 软件 > 定时器）
- **S 态 PLIC 上下文 + WFI（v0.15）** — 每个 hart 有 M/S 两个 PLIC 上下文（M = 2h、S = 2h+1，和 QEMU virt 一致）：M 上下文拉 `mip.MEIP`，S 上下文拉 `mip.SEIP`/`sip.SEIP`。`mideleg[9] = 1` 时 S 外部中断直接进 `stvec`（`scause = 0x80000009`），claim/complete 也走 S 上下文；没委托时由 M 态用 `mie.SEIE` 接管。`wfi` 在没有已使能中断时真的停下来（`hart->waiting`），中断到来后恢复；所有核都在 WFI 且没有定时器/外部源时判定死锁并停机，有 `mtimecmp` 则直接把 `mtime` 快进到最近的一次到期
- **S 态定时器中断（v0.16）** — 实现 Sstc 的 `stimecmp`/`stimecmph`（64 位比较值，RV32 拆两个 CSR）：`mtime` 追平比较值就把 `mip.STIP`/`sip.STIP` 拉成电平。`mideleg[5] = 1` 时 S 定时器中断直接进 `stvec`（`scause = 0x80000005`），否则由 M 态用 `mie.STIE` 接管；M 态软件也可以直接写 `mip.STIP` 转发。委托出去的中断不打断 M 态，`sip.STIP` 对 S 态只读，WFI 的唤醒条件和死锁快进都算上了 `stimecmp`
- **电平设备与 complete 重报（v0.17）** — 设备可以按电平方式拉线（`plic_set_level` / 电平寄存器），线一直为高时 claim 先清 pending，`complete` 时网关重新放行、pending 再次置起，中断会重复投递到 handler。驱动必须在 complete 之前把设备的中断清掉，顺序反了就是中断风暴。边沿设备（门铃）没有线电平，complete 后不会重报
- **写缓冲与弱内存模型（v0.18）** — 每个 hart 一个 8 项写缓冲，`--mem-model weak` 时 store 先入缓冲、`sb_lag + (addr>>2)%16` 条指令后才写进共享内存；本 hart 的 load 从写缓冲转发（store-to-load forwarding），别的 hart 只能看到已提交的写。`fence` 把缓冲刷干净，原子指令/trap 边界也会强制提交。`--mem-model sc` 保持原来的顺序一致。两个 litmus test：store buffering 在 SC 下是 `11`、弱模型下是 `00`；message passing 有 `fence` 是 `11`、去掉 fence 是 `10`
- **读缓冲与 load buffering（v0.19）** — `--lb-lag N` 打开 load 缓冲：整数 load 发射时只登记 `(rd, 物理地址, 宽度, 符号扩展)`，过 `lb_lag + (addr>>2)%16` 条指令才从内存取值写回；写回前目标寄存器在记分牌上标 pending，后续指令只要读它就原地停住（PC 不动），同一个 rd 不会重复发射。等待期间同 hart 后面的 store 可以照常提交，因此 load 能看到"比自己晚"的 store。`fence`、原子指令、trap 边界和 WFI 都会强制排空读缓冲。配了一个 load buffering litmus：SC 下是 `00`，弱模型默认（`lb_lag=0`）也是 `00`，`--lb-lag 16` 时两个 load 都读到对方后来的 store，输出 `11` —— 这个结果在 SC 下会形成 `Lx<Sy<Ly<Sx<Lx` 的环，是非法的。浮点 load、MMIO 和 x0 目标不进缓冲
- **fence 谓词（v0.20）** — `fence` 不再无条件全刷：按编码里的 `fm/pred/succ`（每段 `I=8 O=4 R=2 W=1`）决定动作，`pred` 含 `R/I` 排空读缓冲，含 `W/O` 刷新写缓冲，`succ` 不改变动作（fence 点一次等到位，之后的操作天然在后）；`fence.i` 仍全刷。新增两组 litmus 对照：message passing 里 `fence r,r` 只约束 load-load，store 顺序仍然乱，输出 `10`；换成 `fence w,w` 保 store-store，输出 `11`。store buffering 两边各插一条 `fence rw,rw` 后也从 `00` 变成 `11`
- **简化 CLINT** — `mtime` 全机器共享，`mtimecmp` 每个 hart 一个（8 字节步长，地址与真实 CLINT 一致），可做定时器中断
- **自测** — 解码 49 项 + RV32C 展开/执行 33 项 + 执行 65 项 + 多 hart/IPI 34 项 + PLIC 119 项 + trap 20 项 + S 态/委托 30 项 + S 定时器 31 项 + Sv32 33 项 + RV32F/D/FMA 67 项 + 写缓冲/读缓冲 41 项，共 513 项，`make test` 全过

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

# Sv32 demo：M 态建页表 → mret 进 U 态 → 缺页 / 非法指令
make sv32-demo

# S 态 demo：U 态 ecall 委托给 S 态 handler → sret 返回
make s-demo

# RV32A demo：LR/SC 成功/失败 + AMOADD/AMOMAXU
make atomic-demo

# RV32C demo：整段程序只用 16 位指令，打印一行字符串
make rvc-demo

# RV32F demo：浮点加减乘除、开方、转换、存回内存
make f-demo

# RV32D demo：双精度加减乘除、开方、转换、融合乘加、64 位内存往返
make d-demo

# mstatus.FS demo：关闭浮点 → 非法指令；开启 → 执行后变 Dirty
make fs-demo

# 双 hart demo：两个核各自读 mhartid 分流，用 amoadd 把共享计数器加到 50
make multihart-demo

# 核间中断 demo：hart1 写 CLINT msip 给 hart0 发 IPI，handler 里清掉并打印 IPI55
make ipi-demo

# PLIC demo：设备同时拉起 3 条中断，handler 按优先级 claim/complete
make plic-demo

# S 态外部中断 + WFI：M 态委托 SEI，S 态 WFI 等另一个核拉门铃
make plic-s-demo

# S 态定时器 demo：stimecmp 到点 → STIP → stvec，WFI 等待并打印 S123
make stip-demo

# 电平设备 demo：complete 后线还高 → 重报，第 3 次清线后停下，打印 L123
make level-demo

# 写缓冲 litmus：SB 弱模型打印 00（SC 模式是 11）
make sb-demo

# message passing litmus：有 fence 打印 11，去掉 fence 打印 10
make mp-demo

# load buffering litmus：SC 打印 00，--lb-lag 16 打印 11
make lb-demo

# 也可以手动选内存模型和缓冲延迟跑同一个程序
./build/rvemu --harts 2 --mem-model sc   build/litmus_sb.elf
./build/rvemu --harts 2 --mem-model weak build/litmus_sb.elf
./build/rvemu --harts 2 --mem-model weak --lb-lag 16 build/litmus_lb.elf

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
| `r` | 打印 32 个整数寄存器、32 个浮点寄存器和 CSR |
| `x <addr> [n]` | 按字节 dump 内存 |
| `d [addr] [n]` | 反汇编 n 条指令 |
| `b <addr>` / `bl` / `bd <addr>` / `bc` | 断点增删查清 |
| `q` | 退出 |

## 架构

```
main.c → emu.c (fetch→decode→execute)
           ├── elf.c       ELF 解析器
           ├── decode.c    指令解码 + RV32C 展开 + 反汇编
           ├── execute.c   执行引擎 (RV32I + RV32M + RV32A + ecall)
           ├── fpu.c       RV32F/D 浮点运算 + FMA + fflags/frm
           ├── debug.c     交互式调试器
           ├── trap.c      trap 框架 (异常/中断/mret) + 简化 CLINT
           ├── plic.c      PLIC 外部中断控制器 + 虚拟设备门铃
           ├── mmu.c       Sv32 分页 (页表遍历 / 权限 / A-D 位)
           ├── regfile.c   寄存器文件 (x0-x31 + f0-f31 + PC)
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

`tests/gen_sv32_demo.py` 演示分页和特权级。M 态先在物理内存里建好两级页表（代码页 identity 映射、数据页、只读页、一个 4MB 大页、一个故意不映射的地址），写 satp 后用 `mret` 进入 U 态。U 态依次做五件事，每一件的成功或陷入都会打一个字符：

```
$ make sv32-demo
WSIPF
[SYSTEM] exit(0)
```

- `W` — 往 0x40000000（VA）写数再读回，经过页表翻译落到物理页 0x83000
- `S` — 从 4MB 大页 0xC0010000 读到一个字（一级页表叶子项）
- `I` — U 态执行 `csrr mstatus`，非法指令陷入（mcause 2），handler 跳过它继续
- `P` — 往只读页 0x40001000 写，store page fault（mcause 15），mtval 是虚拟地址
- `F` — 读未映射的 0x50000000，load page fault（mcause 13），handler 把特权级改回 M 后退出

`tests/gen_s_demo.py` 演示 S 态和异常委托。M 态只设置 `stvec` 和 `medeleg[8]`（只委托 U 态 ecall），然后 `mret` 进 U 态。U 态执行 `ecall` 时，trap 不经过 M 态，直接进入 S 态 handler；handler 检查 `scause == 8`、把 `sepc` 加 4、`sret` 回到 U 态：

```
$ make s-demo
S8U
[SYSTEM] ebreak: halted
```

- `S` — S 态 handler 开始执行
- `8` — `scause` 是 8（ecall from U），说明委托生效
- `U` — `sret` 正常回到 U 态，下一条指令继续执行

`tests/gen_atomic_demo.py` 演示 RV32A。先在数据页写 5，`lr.w` 建立保留集，`sc.w` 写入 7 成功；再用普通 `sw` 打断保留集，第二次 `sc.w` 必须返回 1；最后用 `amoadd.w` 和 `amomaxu.w` 验证读-改-写和旧值返回：

```
$ make atomic-demo
SRAM
[SYSTEM] exit(0)
```

- `S` — LR/SC 成功
- `R` — 普通 store 打断保留集，SC 失败
- `A` — `amoadd.w` 返回旧值 9，mem 变成 16
- `M` — `amomaxu.w` 返回旧值 16（max(16, 7) = 16）

`tests/gen_f_demo.py` 演示 RV32F。数据页里放 1.5f、2.25f、9.0f，用 `flw` 读进 f 寄存器，依次算 `fadd`、`fmul`、`fsqrt`、`fcvt.w.s`，最后把结果用 `fsw` 写回内存、再用 `lw` 读原始位验证。每项成功打印一个 `F`：

```
$ make f-demo
FFFFF
[SYSTEM] exit(0)
```

`tests/gen_plic_demo.py` 演示外部中断。程序把 IRQ1/2/3 的优先级分别设成 1/3/2，全部使能、阈值设为 0，然后往虚拟设备的门铃寄存器写 `0b1110`。设备同时拉起三条中断线，CPU 在下一条指令边界进 trap（`mcause = 0x8000000B`）。handler 循环 claim，按优先级依次拿到 2、3、1，每条处理完写 claim 寄存器 complete；claim 返回 0 时置完成标志并 `mret`：

```
$ make plic-demo
PLIC:231
[SYSTEM] exit(0)
```

- `2` — IRQ2 优先级最高（3），先报
- `3` — IRQ3 优先级次之（2）
- `1` — IRQ1 最后（1）

`tests/gen_plic_s_demo.py` 把 PLIC 接到 S 态。主核在 M 态设 `mideleg[9]`、配置自己的 S 上下文（ctx1），然后 `mret` 进 S 态，打印 `S` 后执行 `wfi`；辅助核空转一会儿，往门铃写 IRQ2。SEIP 拉起后主核从 WFI 醒来，直接陷进 `stvec`，handler 从 S 上下文 claim/complete，`sret` 回到 WFI 后面继续打印：

```
$ make plic-s-demo
S:2
[SYSTEM] ebreak: halted
```

`tests/gen_stip_demo.py` 演示 S 态定时器。M 态设 `stvec`、`mideleg[5]`、`sie.STIE`，把 `stimecmp` 设成 40，然后 `mret` 进 S 态。S 态打印 `S` 后在 `wfi` 里等；`mtime` 追平 `stimecmp` 时 `mip.STIP` 拉起，直接陷进 `stvec`（`scause = 0x80000005`）。handler 每次打一个序号、把 `stimecmp` 往后推 20；第三次之后关掉定时器并把 `sepc` 指到出口，`sret` 直接回主流程打印换行：

```
$ make stip-demo
S123
[SYSTEM] ebreak: halted
```

`tests/gen_level_demo.py` 演示电平设备。主流程把 IRQ5 的线拉高，handler 每次 claim 后打印一个序号；前两次 complete 时线还是高的，PLIC 立刻重新置 pending，下一条指令又进 handler。第三次先把电平寄存器写成 0，再 complete，中断才停下来：

```
$ make level-demo
L123
[SYSTEM] ebreak: halted
```

这就是真实驱动里"先清设备、再 complete"的顺序要求。反过来写代码能跑通边沿设备，换成电平设备就会一直卡在中断里。

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
| Sv32 分页 (4KB 页 / 4MB 大页 / 权限 / A-D) | ✅ |
| U 态 + 特权级切换 (MPP / mret / 特权指令陷入) | ✅ |
| S 态 + 异常委托 (medeleg / mideleg / stvec / sret / SUM) | ✅ |
| RV32A 原子指令 (LR/SC + 11 条 AMO) | ✅ |
| RV32F 单精度浮点 (F 扩展 + fcsr) | ✅ |
| RV32C 压缩指令 (16 位展开) | ✅ |
| RV32D 双精度浮点 (NaN boxing) | ✅ |
| 融合乘加 FMA.S / FMA.D | ✅ |
| 多 hart + 跨核原子 + IPI (CLINT msip) | ✅ |
| PLIC 外部中断 (优先级 / 阈值 / claim / complete) | ✅ |
| S 态定时器中断 (Sstc stimecmp / STIP 委托 / WFI) | ✅ |
| 电平设备 + complete 重报 (中断风暴) | ✅ |

## License

MIT
