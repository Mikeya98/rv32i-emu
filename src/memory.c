/**
 * @file    memory.c
 * @brief   RV32I Emulator — 内存模型实现
 */

#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void memory_init(memory_t *mem)
{
    mem->size = MEM_SIZE;
    mem->data = (u8 *)calloc(mem->size, 1);
}

void memory_free(memory_t *mem)
{
    free(mem->data);
    mem->data = NULL;
    mem->size = 0;
}

u32 memory_load(memory_t *mem, u32 addr, const u8 *src, u32 len)
{
    if (addr + len > mem->size) {
        u32 clip = (addr < mem->size) ? (mem->size - addr) : 0;
        len = clip;
    }
    memcpy(mem->data + addr, src, len);
    return len;
}

/* =========================== 带边界检查的读写 =========================== */

bool memory_read8(memory_t *mem, u32 addr, u8 *val)
{
    if (addr >= mem->size) return false;
    *val = mem->data[addr];
    return true;
}

bool memory_read16(memory_t *mem, u32 addr, u16 *val)
{
    if (addr + 2 > mem->size) return false;
    *val = mem->data[addr] | ((u16)mem->data[addr + 1] << 8);
    return true;
}

bool memory_read32(memory_t *mem, u32 addr, u32 *val)
{
    if (addr + 4 > mem->size) return false;
    *val = mem->data[addr]
         | ((u32)mem->data[addr + 1] << 8)
         | ((u32)mem->data[addr + 2] << 16)
         | ((u32)mem->data[addr + 3] << 24);
    return true;
}

bool memory_write8(memory_t *mem, u32 addr, u8 val)
{
    /* MMIO: UART 输出 */
    if (addr == CSR_MMIO_UART) {
        putchar(val);
        return true;
    }
    if (addr >= mem->size) return false;
    mem->data[addr] = val;
    return true;
}

bool memory_write16(memory_t *mem, u32 addr, u16 val)
{
    if (addr + 2 > mem->size) return false;
    mem->data[addr]     = (u8)(val & 0xFF);
    mem->data[addr + 1] = (u8)((val >> 8) & 0xFF);
    return true;
}

bool memory_write32(memory_t *mem, u32 addr, u32 val)
{
    if (addr + 4 > mem->size) return false;
    mem->data[addr]     = (u8)(val & 0xFF);
    mem->data[addr + 1] = (u8)((val >>  8) & 0xFF);
    mem->data[addr + 2] = (u8)((val >> 16) & 0xFF);
    mem->data[addr + 3] = (u8)((val >> 24) & 0xFF);
    return true;
}

/* =========================== 调试 dump =========================== */

void memory_dump(memory_t *mem, u32 start, u32 len)
{
    u32 i;

    if (start >= mem->size) return;
    if (start + len > mem->size) len = mem->size - start;

    printf("Memory dump [0x%08X — 0x%08X]:\n", start, start + len - 1);

    for (i = 0; i < len; i++) {
        if ((i & 0xF) == 0) printf("  0x%08X  ", start + i);
        printf("%02X ", mem->data[start + i]);
        if ((i & 0xF) == 0xF) printf("\n");
    }
    if ((len & 0xF) != 0) printf("\n");
}
