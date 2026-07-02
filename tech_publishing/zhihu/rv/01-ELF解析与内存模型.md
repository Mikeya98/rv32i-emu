# 用纯 C 写一个 RISC-V 模拟器——先让程序能加载进来

> 从零写一个 RISC-V CPU 模拟器 · 一

我最近开始写一个 RISC-V CPU 模拟器。纯 C，40 条 RV32I 指令，零依赖——不需要开发板，不需要交叉编译工具链，clone 下来 `make` 就能跑。

这篇文章是第一篇，讲第一步：怎么把一个 RISC-V 程序从 ELF 文件加载到模拟器的虚拟内存里。

先看效果。用 `riscv-none-elf-gcc` 编译一个最简单的程序，丢进模拟器：

```
$ ./rvemu hello.elf

┌──────────────────────────────────────┐
│  RV32I Emulator v0.1                 │
│  RISC-V RV32I Base Integer ISA       │
└──────────────────────────────────────┘

[ELF] === Header ===
  type      = 2 (ET_EXEC)
  machine   = 243 (EM_RISCV)
  entry     = 0x00010074
  phnum     = 3

[ELF] === Program Headers ===
  Type   Offset     VirtAddr   FileSiz    MemSiz     Flags
  1      0x00001000 0x00010000 0x000000BC 0x000000BC R-X
          → loaded 188 bytes to [0x00010000 — 0x000100BB]
  1      0x00002000 0x00011000 0x00000010 0x00000018 RW-
          → loaded 16 bytes to [0x00011000 — 0x0001100F]
          → bss zeroed [0x00011010 — 0x00011017] (8 bytes)

[ELF] Load OK. entry=0x00010074, 2 PT_LOAD segments loaded.
```

ELF 文件被解析，代码段和数据段加载到模拟内存，入口地址已设置。下一步只需要从 `0x00010074` 开始逐条取指、解码、执行。

这篇文章就讲这个加载过程是怎么实现的。

---

## ELF 文件长什么样

ELF（Executable and Linkable Format）是 Linux 系统上可执行文件的标准格式。一个 RISC-V 的 ELF 文件结构如下：

```
┌──────────────────────┐  0x00
│     ELF Header       │  52 字节 — 魔数、入口地址、段表位置
├──────────────────────┤
│   Program Headers    │  每个 32 字节 — 描述一个"段"怎么加载
│   (LOAD segment 0)   │
│   (LOAD segment 1)   │
├──────────────────────┤
│     .text (代码)      │  ← 第一个 LOAD 段指向这里
├──────────────────────┤
│     .data (数据)      │  ← 第二个 LOAD 段指向这里
├──────────────────────┤
│   Section Headers    │  链接/调试信息 (可选，模拟器不需要)
└──────────────────────┘
```

对模拟器来说，只需要两样东西：**ELF Header**（告诉你在哪找到入口地址）和 **Program Headers**（告诉你怎么把数据加载到内存）。Section Headers 是给链接器和调试器用的，不需要管。

### ELF Header

ELF Header 在文件最开头，52 字节。用 C 的结构体表示就是：

```c
typedef struct {
    u8   e_ident[16];   // 魔数: 0x7F 'E' 'L' 'F'
    u16  e_type;        // ET_EXEC=2 (可执行文件)
    u16  e_machine;     // EM_RISCV=243
    u32  e_version;
    u32  e_entry;       // ★ 入口地址 — CPU 从哪开始执行
    u32  e_phoff;       // Program Header 表在文件中的偏移
    u32  e_shoff;       // Section Header 表偏移 (不用)
    u32  e_flags;
    u16  e_ehsize;      // ELF Header 大小 (52)
    u16  e_phentsize;   // 每个 Program Header 的大小 (32)
    u16  e_phnum;       // ★ Program Header 的数量
    u16  e_shentsize;   // Section Header 大小 (不用)
    u16  e_shnum;       // Section Header 数量 (不用)
    u16  e_shstrndx;    // Section 名字符串表索引 (不用)
} elf32_ehdr_t;
```

关键字段就是 `e_entry`（入口地址）、`e_phoff`（段表偏移）、`e_phnum`（段数量）。其他的 `e_ident` 用来验证这是不是一个合法的 ELF 文件（前 4 字节必须是 `\x7FELF`），`e_machine` 用来确认是不是 RISC-V（243 = `EM_RISCV`）。

### Program Header

每个 Program Header 32 字节，描述一组要加载到内存的数据：

```c
typedef struct {
    u32  p_type;    // PT_LOAD=1 (可加载段)
    u32  p_offset;  // 在 ELF 文件中的偏移
    u32  p_vaddr;   // ★ 加载到内存的哪个地址
    u32  p_paddr;   // 物理地址（和 vaddr 相同）
    u32  p_filesz;  // 文件中占多少字节
    u32  p_memsz;   // ★ 内存中占多少字节 (>= filesz)
    u32  p_flags;   // R=4, W=2, X=1
    u32  p_align;   // 对齐
} elf32_phdr_t;
```

`p_filesz` 是从文件里读多少字节，`p_memsz` 是在内存里占多少字节。如果 `p_memsz > p_filesz`，多出来的部分就是 BSS 段（未初始化的全局变量），填 0。

一个典型的 RISC-V ELF 文件通常有 2 个 LOAD 段：
- 第一个是代码段（`.text`），flags 是 `R-X`（可读可执行）
- 第二个是数据段（`.data` + `.bss`），flags 是 `RW-`（可读可写）

---

## 怎么解析

代码很简单——读整个文件进内存（ELF 通常几十到几百 KB，直接全读），然后按偏移把结构体字段取出来：

```c
/* 1. 验证 magic */
u32 magic = rd32le(file_buf);
if (magic != 0x464C457F) {  /* "\x7FELF" */
    printf("Not a valid ELF\n");
    return 0;
}

/* 2. 解析 ELF Header */
parse_ehdr(file_buf, &ehdr);    // 逐字段读

if (ehdr.e_type != ET_EXEC)  { /* 只处理可执行文件 */ }
if (ehdr.e_machine != EM_RISCV) { /* 确认是 RISC-V */ }

/* 3. 遍历 Program Headers */
for (i = 0; i < ehdr.e_phnum; i++) {
    parse_phdr(file_buf + ehdr.e_phoff + i * 32, &ph);

    if (ph.p_type != PT_LOAD) continue;

    /* 把文件内容复制到模拟内存 */
    memory_load(mem, ph.p_vaddr,
                file_buf + ph.p_offset, ph.p_filesz);

    /* BSS: memsz > filesz 的部分填 0 */
    if (ph.p_memsz > ph.p_filesz) {
        memset(mem->data + ph.p_vaddr + ph.p_filesz,
               0, ph.p_memsz - ph.p_filesz);
    }
}
```

没有依赖 `libelf`。ELF 文件格式本身不复杂——它就是一个带元数据的二进制 blob。

---

## 模拟器的内存模型

目前是最简单的方案：64KB 的 `calloc` 数组。

```c
typedef struct {
    u8 *data;
    u32 size;     /* 64 * 1024 */
} memory_t;

void memory_init(memory_t *mem) {
    mem->size = 64 * 1024;
    mem->data = (u8 *)calloc(mem->size, 1);
}
```

地址就是数组下标。`memory_read32(addr)` 从 `data[addr]` 开始按小端读 4 个字节。`memory_write32(addr, val)` 反过来写 4 个字节。每次读写都检查边界，越界直接报错。

64KB 对于跑 RV32I 测试程序来说绰绰有余——一个简单的斐波那契或者冒泡排序的 ELF，整个文件不超过 10KB。

另外留了一个 MMIO 的出口：写入地址 `0x20000000` 的字节会被直接打印到终端。这样后续可以用 `sw` 指令向这个地址写数据来实现 `printf` 效果。

---

## 为什么自己写一个

RISC-V 在国内这几年热度很高，国产替代的背景下讨论尤其多。但大多数人看的是指令集手册或者别人的分析文章。自己动手写一个模拟器，每个指令怎么解码、寄存器怎么变化、PC 怎么跳转，全都变成你能跑的代码，理解深度完全不同。

而且这个东西的门槛其实很低。你不需要 FPGA 板子，不需要 RISC-V 开发板，甚至不需要交叉编译工具链——有一个 gcc 就行了。模拟器本身是纯 C 写的 x86 程序，RISC-V 的测试程序可以用 `riscv-none-elf-gcc` 编译（或者在线的 RISC-V 编译器生成）。

下一篇文章讲指令解码——怎样把 32-bit 的机器码变成 `add x1, x2, x3` 这样的人类能读的形式。

---

代码在 GitHub：[github.com/Mikeya98/rv32i-emu](https://github.com/Mikeya98/rv32i-emu)
