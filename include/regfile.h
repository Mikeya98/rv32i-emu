/**
 * @file    regfile.h
 * @brief   RV32I Emulator — 寄存器文件
 *
 * x0 恒为 0 (硬件连线), x1-x31 通用寄存器, PC 独立。
 */

#ifndef _RVEMU_REGFILE_H
#define _RVEMU_REGFILE_H

#include "types.h"

#define REG_COUNT  32
#define REG_ZERO    0    /* x0 — zero register */

typedef struct {
    u32  regs[REG_COUNT];  /* x0..x31 */
    u32  pc;               /* 程序计数器 (当前指令地址) */
} regfile_t;

/* --- API --- */

void  regfile_init(regfile_t *rf);

/* 读/写 (x0 写被忽略, 读返回 0) */
u32   reg_read (regfile_t *rf, u32 idx);
void  reg_write(regfile_t *rf, u32 idx, u32 val);

/* 调试: 打印全部寄存器 */
void  regfile_dump(regfile_t *rf);

#endif /* _RVEMU_REGFILE_H */
