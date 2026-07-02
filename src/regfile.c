/**
 * @file    regfile.c
 * @brief   RV32I Emulator — 寄存器文件实现
 */

#include "regfile.h"
#include <stdio.h>
#include <string.h>

/* ABI 寄存器名称 */
static const char *abi_names[] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

void regfile_init(regfile_t *rf)
{
    memset(rf->regs, 0, sizeof(rf->regs));
    rf->pc = 0;
}

u32 reg_read(regfile_t *rf, u32 idx)
{
    if (idx >= REG_COUNT) return 0;
    return rf->regs[idx];
}

void reg_write(regfile_t *rf, u32 idx, u32 val)
{
    if (idx == 0 || idx >= REG_COUNT) return;  /* x0 忽略写入 */
    rf->regs[idx] = val;
}

void regfile_dump(regfile_t *rf)
{
    int i;

    printf("┌──────┬──────────┬──────────┐\n");
    printf("│ reg  │  ABI     │   value  │\n");
    printf("├──────┼──────────┼──────────┤\n");

    for (i = 0; i < 32; i++) {
        printf("│ x%-3u │ %-8s │ 0x%08X │\n",
               i, abi_names[i], rf->regs[i]);
    }
    printf("├──────┴──────────┴──────────┤\n");
    printf("│ pc                       │ 0x%08X │\n", rf->pc);
    printf("└──────────────────────────┘\n");
}
