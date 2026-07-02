/**
 * @file    elf.c
 * @brief   RV32I Emulator — 32-bit RISC-V ELF 加载器实现
 *
 * 解析 ELF header + program headers, 将 PT_LOAD 段加载到模拟内存。
 * BSS (memsz > filesz) 的超出部分填 0。
 */

#include "elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================== 内联辅助 =========================== */

static u32 rd32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16le(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

/* 从 buffer 逐字段填充 ehdr (ELF 是小端, x86 也是小端, 但为了可移植性手动解码) */
static void parse_ehdr(const u8 *buf, elf32_ehdr_t *ehdr)
{
    ehdr->e_type      = rd16le(buf + 16);
    ehdr->e_machine   = rd16le(buf + 18);
    ehdr->e_version   = rd32le(buf + 20);
    ehdr->e_entry     = rd32le(buf + 24);
    ehdr->e_phoff     = rd32le(buf + 28);
    ehdr->e_shoff     = rd32le(buf + 32);
    ehdr->e_flags     = rd32le(buf + 36);
    ehdr->e_ehsize    = rd16le(buf + 40);
    ehdr->e_phentsize = rd16le(buf + 42);
    ehdr->e_phnum     = rd16le(buf + 44);
    ehdr->e_shentsize = rd16le(buf + 46);
    ehdr->e_shnum     = rd16le(buf + 48);
    ehdr->e_shstrndx  = rd16le(buf + 50);
}

static void parse_phdr(const u8 *buf, elf32_phdr_t *phdr)
{
    phdr->p_type   = rd32le(buf + 0);
    phdr->p_offset = rd32le(buf + 4);
    phdr->p_vaddr  = rd32le(buf + 8);
    phdr->p_paddr  = rd32le(buf + 12);
    phdr->p_filesz = rd32le(buf + 16);
    phdr->p_memsz  = rd32le(buf + 20);
    phdr->p_flags  = rd32le(buf + 24);
    phdr->p_align  = rd32le(buf + 28);
}

/* =========================== 公共 API =========================== */

u32 elf_load(const char *path, memory_t *mem, elf_info_t *info)
{
    FILE   *fp;
    u8     *file_buf;
    u32     file_size;
    elf32_ehdr_t ehdr;
    u32     i;
    u32     entry = 0;

    if (!path || !mem || !info) return 0;
    memset(info, 0, sizeof(*info));

    /* 1. 读入整个文件 */
    fp = fopen(path, "rb");
    if (!fp) {
        printf("[ELF] Cannot open: %s\n", path);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    file_size = (u32)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    file_buf = (u8 *)malloc(file_size);
    if (!file_buf) {
        printf("[ELF] malloc failed (%u bytes)\n", file_size);
        fclose(fp);
        return 0;
    }
    if (fread(file_buf, 1, file_size, fp) != file_size) {
        printf("[ELF] fread failed\n");
        free(file_buf);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    /* 2. 验证 ELF magic */
    if (file_size < 52 || rd32le(file_buf) != ELF_MAGIC) {
        printf("[ELF] Not a valid ELF file (magic mismatch)\n");
        free(file_buf);
        return 0;
    }

    /* 3. 解析 ELF header */
    parse_ehdr(file_buf, &ehdr);

    printf("[ELF] === Header ===\n");
    printf("  type      = %u (ET_EXEC=%u)\n", ehdr.e_type, ET_EXEC);
    printf("  machine   = %u (EM_RISCV=%u)\n", ehdr.e_machine, EM_RISCV);
    printf("  entry     = 0x%08X\n", ehdr.e_entry);
    printf("  phoff     = %u\n", ehdr.e_phoff);
    printf("  phnum     = %u\n", ehdr.e_phnum);
    printf("  phentsize = %u\n", ehdr.e_phentsize);

    if (ehdr.e_type != ET_EXEC) {
        printf("[ELF] Only ET_EXEC supported (got type=%u)\n", ehdr.e_type);
        free(file_buf);
        return 0;
    }
    if (ehdr.e_machine != EM_RISCV) {
        printf("[ELF] Only EM_RISCV supported (got machine=%u)\n", ehdr.e_machine);
        free(file_buf);
        return 0;
    }

    /* 4. 解析 Program Headers */
    printf("[ELF] === Program Headers ===\n");
    printf("  %-6s %-10s %-10s %-10s %-10s %s\n",
           "Type", "Offset", "VirtAddr", "FileSiz", "MemSiz", "Flags");

    entry = ehdr.e_entry;
    info->text_start = 0xFFFFFFFF;
    info->data_start = 0xFFFFFFFF;
    info->phdr_count = 0;

    for (i = 0; i < ehdr.e_phnum; i++) {
        elf32_phdr_t ph;
        u32 ph_offset = ehdr.e_phoff + i * ehdr.e_phentsize;

        if (ph_offset + 32 > file_size) break;

        parse_phdr(file_buf + ph_offset, &ph);

        printf("  %-6u 0x%08X 0x%08X %-10u %-10u %c%c%c\n",
               ph.p_type, ph.p_offset, ph.p_vaddr,
               ph.p_filesz, ph.p_memsz,
               (ph.p_flags & PF_R) ? 'R' : '-',
               (ph.p_flags & PF_W) ? 'W' : '-',
               (ph.p_flags & PF_X) ? 'X' : '-');

        if (ph.p_type != PT_LOAD) continue;

        /* 检查文件范围 */
        if (ph.p_offset + ph.p_filesz > file_size) {
            printf("[ELF] PT_LOAD #%u: offset+filesz out of range\n", i);
            continue;
        }

        /* 加载到模拟内存 */
        u32 loaded = memory_load(mem, ph.p_vaddr, file_buf + ph.p_offset, ph.p_filesz);
        printf("        → loaded %u bytes to [0x%08X — 0x%08X]\n",
               loaded, ph.p_vaddr, ph.p_vaddr + loaded - 1);

        /* BSS: memsz > filesz 的部分填 0 */
        if (ph.p_memsz > ph.p_filesz) {
            u32 bss_start = ph.p_vaddr + ph.p_filesz;
            u32 bss_size  = ph.p_memsz - ph.p_filesz;
            /* 简单 memset (memory_load 不会超出范围) */
            if (bss_start + bss_size <= mem->size) {
                memset(mem->data + bss_start, 0, bss_size);
                printf("        → bss zeroed [0x%08X — 0x%08X] (%u bytes)\n",
                       bss_start, bss_start + bss_size - 1, bss_size);
            }
            info->bss_start = bss_start;
            info->bss_end   = bss_start + bss_size;
        }

        /* 更新 info 中的段范围 */
        if (ph.p_flags & PF_X) {
            if (ph.p_vaddr < info->text_start) info->text_start = ph.p_vaddr;
            u32 end = ph.p_vaddr + ph.p_memsz;
            if (end > info->text_end) info->text_end = end;
        }
        if (ph.p_flags & PF_W) {
            if (ph.p_vaddr < info->data_start) info->data_start = ph.p_vaddr;
            u32 end = ph.p_vaddr + ph.p_memsz;
            if (end > info->data_end) info->data_end = end;
        }

        info->phdr_count++;
    }

    free(file_buf);

    if (info->phdr_count == 0) {
        printf("[ELF] No PT_LOAD segments found.\n");
        return 0;
    }

    printf("[ELF] Load OK. entry=0x%08X, %u PT_LOAD segments loaded.\n",
           entry, info->phdr_count);
    return entry;
}
