# RV32I Emulator — 开发与知乎发布计划

## 项目定位

用纯 C 写一个 RISC-V RV32I 指令集模拟器，面向知乎嵌入式/AI 读者群体。
**发布优先**——每一阶段的代码服务于对应文章的展示效果。

## 技术架构

```
┌─────────────────────────────────────┐
│         main.c (入口/参数解析)        │
├─────────────────────────────────────┤
│     emu.c (顶层调度: fetch→decode→exec) │
├──────────┬──────────┬───────────────┤
│  elf.c   │ decode.c │  execute.c     │
│ ELF 解析  │ 指令解码  │  执行引擎       │
├──────────┴──────────┴───────────────┤
│  regfile.c    memory.c     csr.c     │
│  寄存器文件     内存模型     CSR 控制   │
└─────────────────────────────────────┘
```

## 开发里程碑

### Phase 1: 骨架 (第 1-2 篇的基础)
- [x] 项目结构 + Makefile
- [x] types.h / memory.h/c / regfile.h/c
- [x] ELF parser (32-bit RISC-V ELF) — elf.h/c 完成
- [x] decode 框架 — 6 种格式完整解析
- [x] CSR 框架 — 最小 Machine-mode CSR
- [x] GitHub 仓库建好 + 代码推送: `github.com/Mikeya98/rv32i-emu`
- [x] 知乎第 1 篇撰写完毕: ELF 解析与内存模型
- [ ] decode self-test — 手写指令机器码验证
- [ ] execute.c — 执行引擎

### Phase 2: 核心执行 (第 3-5 篇)
- [ ] execute.c — RV32I 40 条指令逐条实现
  - ALU: add/sub/sll/slt/sltu/xor/srl/sra/or/and
  - ALUI: addi/slli/slti/sltiu/xori/srli/srai/ori/andi
  - LOAD: lb/lh/lw/lbu/lhu
  - STORE: sb/sh/sw
  - BRANCH: beq/bne/blt/bge/bltu/bgeu
  - JAL/JALR/LUI/AUIPC
  - SYSTEM: ecall/ebreak/csr*
- [ ] 主循环: while(running) { fetch → decode → execute → pc+=4 }

### Phase 3: 控制流 + 特权 (第 6 篇)
- [ ] 分支/跳转实现 (PC 修改)
- [ ] CSR 读写指令
- [ ] ecall 处理 (exit/mret)
- [ ] 异常处理框架

### Phase 4: 完整测试 (第 7 篇)
- [ ] 用 riscv-gnu-toolchain 编译测试程序
- [ ] 跑通一个简单程序 (斐波那契/冒泡排序)
- [ ] 输出结果验证

## 知乎发布计划 (7 篇)

| 篇 | 标题 | 代码阶段 | 可视化元素 |
|----|------|---------|-----------|
| 1 | 我写了一个 RISC-V CPU 模拟器——ELF 解析与内存模型 | Phase 1 | ELF 段布局图, 内存映射图 |
| 2 | 指令解码：把 32-bit 机器码变成人类能看懂的东西 | Phase 1 | 解码表, 各格式字段标注图 |
| 3 | 寄存器文件 + 第一个能跑的指令：addi | Phase 2 | 寄存器 dump 前后对比 |
| 4 | Load/Store：CPU 怎么读写内存 | Phase 2 | 内存读写前后 dump |
| 5 | 分支和跳转：if/for/while 在 CPU 眼里是什么 | Phase 2 | 执行流程追踪图 |
| 6 | 特权架构：CSR、异常、ecall——操作系统的基础 | Phase 3 | 异常处理流程图 |
| 7 | 跑通一个真实程序 + 系列总结 | Phase 4 | 终端运行截图 |

## 流量策略

1. **每篇标题独立可点击**（MOS 后期经验：独立标题比系列编号更吸引非系列读者）
2. **每篇有可视化产物**：寄存器 dump、内存对比、执行追踪
3. **首篇发在知乎问答下**（"如何从零开始写一个 CPU 模拟器"之类的问题，问答流量 > 专栏文章）
4. **代码仓库 README 有 GIF/截图**，作为文章引流终点

## 关键差异 (vs MOS 系列)

| | MOS | RV32I Emu |
|----|-----|-----------|
| 平台 | ARM + QEMU | x86 原生，任何人能跑 |
| 门槛 | 需要交叉编译工具链 | 只需要 gcc |
| 受众 | 嵌入式工程师 | 嵌入式 + 软工 + 学生 |
| 可视化 | 终端文字 | 寄存器表 + 内存图 + 执行追踪 |
| 篇数 | 12 篇 | 7 篇 (更紧凑) |
| 定位 | 学习直播 | 科普展示 |

## 文件结构

```
riscv-emu/
├── Makefile
├── PLAN.md              ← 本文件
├── include/
│   ├── types.h
│   ├── memory.h
│   ├── regfile.h
│   ├── decode.h
│   ├── execute.h        (待实现)
│   ├── elf.h             (待实现)
│   ├── csr.h
│   └── emu.h
├── src/
│   ├── main.c
│   ├── memory.c
│   ├── regfile.c
│   ├── decode.c
│   ├── execute.c         (待实现)
│   ├── elf.c             (待实现)
│   ├── csr.c
│   └── emu.c
├── tests/
│   └── (RISC-V 测试 ELF)
└── tech_publishing/
    └── zhihu/
        └── rv/
            ├── 01-ELF解析与内存模型.md
            ├── 02-指令解码.md
            ├── ...
            └── 07-跑通真实程序.md
```
