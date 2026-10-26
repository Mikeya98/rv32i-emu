/**
 * @file    memory.h
 * @brief   RV32I Emulator — 内存模型
 *
 * 1MB 扁平地址空间 (0x00000000 — 0x000FFFFF)。
 * 按字节寻址，支持 8/16/32-bit load/store。
 *
 * 早期是 64KB，但 RISC-V 链接脚本通常把代码放在 0x00010000，
 * 刚好越过 64KB 边界，ELF 加载时整段被裁掉。改成 1MB 后
 * 常见的裸机程序地址都能放下。
 */

#ifndef _RVEMU_MEMORY_H
#define _RVEMU_MEMORY_H

#include "types.h"

#define MEM_SIZE  (1024 * 1024)   /* 1MB */

/* 内存区域 */
typedef struct {
    u8   *data;        /* 字节数组 */
    u32   size;        /* 总字节数 */
} memory_t;

/* --- API --- */

void  memory_init(memory_t *mem);
void  memory_free(memory_t *mem);

/* 从文件 (ELF/二进制) 加载到指定地址 */
u32   memory_load(memory_t *mem, u32 addr, const u8 *src, u32 len);

/* 读写 (带边界检查, 失败返回 false) */
bool  memory_read8 (memory_t *mem, u32 addr, u8  *val);
bool  memory_read16(memory_t *mem, u32 addr, u16 *val);
bool  memory_read32(memory_t *mem, u32 addr, u32 *val);
bool  memory_write8 (memory_t *mem, u32 addr, u8   val);
bool  memory_write16(memory_t *mem, u32 addr, u16  val);
bool  memory_write32(memory_t *mem, u32 addr, u32  val);

/* 调试: dump 指定范围 */
void  memory_dump(memory_t *mem, u32 start, u32 len);

#endif /* _RVEMU_MEMORY_H */
