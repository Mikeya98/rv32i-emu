/**
 * @file    elf.h
 * @brief   RV32I Emulator — 32-bit RISC-V ELF 加载器
 *
 * 只支持 ET_EXEC (可执行文件), EM_RISCV (0xF3),
 * 需要至少一个 PT_LOAD 段。
 * 不依赖 libelf — 直接读二进制结构体。
 */

#ifndef _RVEMU_ELF_H
#define _RVEMU_ELF_H

#include "types.h"

/* =========================== ELF 常量 =========================== */
#define ELF_MAGIC    0x464C457F  /* "\x7FELF" 小端 */

#define EM_RISCV     243         /* RISC-V (0xF3) */
#define ET_EXEC      2           /* 可执行文件 */

#define PT_LOAD      1           /* 可加载段 */
#define PF_X         1
#define PF_W         2
#define PF_R         4

/* =========================== ELF Header (32-bit) =========================== */
typedef struct {
    u8   e_ident[16];   /* 魔数 + 类别 + 端序等 */
    u16  e_type;        /* 文件类型: ET_EXEC=2 */
    u16  e_machine;     /* 目标架构: EM_RISCV=243 */
    u32  e_version;
    u32  e_entry;       /* 入口地址 (虚拟地址) */
    u32  e_phoff;       /* Program Header 表偏移 */
    u32  e_shoff;       /* Section Header 表偏移 */
    u32  e_flags;
    u16  e_ehsize;      /* ELF Header 大小 */
    u16  e_phentsize;   /* Program Header 条目大小 */
    u16  e_phnum;       /* Program Header 条目数 */
    u16  e_shentsize;   /* Section Header 条目大小 */
    u16  e_shnum;       /* Section Header 条目数 */
    u16  e_shstrndx;    /* Section 名字符串表索引 */
} elf32_ehdr_t;

/* =========================== Program Header (32-bit) =========================== */
typedef struct {
    u32  p_type;        /* 段类型: PT_LOAD=1 */
    u32  p_offset;      /* 文件中的偏移 */
    u32  p_vaddr;       /* 虚拟地址 */
    u32  p_paddr;       /* 物理地址 */
    u32  p_filesz;      /* 文件中的大小 */
    u32  p_memsz;       /* 内存中的大小 (>= filesz, 多的部分填零 = .bss) */
    u32  p_flags;       /* 权限: PF_R|PF_W|PF_X */
    u32  p_align;       /* 对齐 */
} elf32_phdr_t;

/* =========================== 加载结果 =========================== */
typedef struct {
    u32  entry;          /* 入口地址 */
    u32  text_start;     /* 第一个可执行段的起始地址 */
    u32  text_end;       /* 文本段结束 */
    u32  data_start;     /* 数据段起始 */
    u32  data_end;       /* 数据段结束 */
    u32  bss_start;      /* BSS 起始 (memsz > filesz 的部分) */
    u32  bss_end;
    u32  phdr_count;     /* PT_LOAD 段数量 */
} elf_info_t;

/* --- API --- */

/**
 * @brief  解析并加载 32-bit RISC-V ELF
 * @param  path     ELF 文件路径
 * @param  mem      目标内存
 * @param  info     输出: 加载摘要信息
 * @return 入口地址, 0 表示失败
 */
u32 elf_load(const char *path, memory_t *mem, elf_info_t *info);

#endif /* _RVEMU_ELF_H */
