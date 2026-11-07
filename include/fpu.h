/**
 * @file    fpu.h
 * @brief   RV32F 单精度浮点扩展
 *
 * FLEN = 32, 32 个独立的 f 寄存器。指令本身仍然走
 * fetch → decode → execute, 只是执行阶段交给 fpu_execute。
 */

#ifndef _RVEMU_FPU_H
#define _RVEMU_FPU_H

#include "types.h"
#include "emu.h"

/**
 * @brief  执行一条 OP-FP 指令 (opcode = 0x53)
 * @return false = 编码不支持 (交给非法指令路径)
 */
bool fpu_execute(emulator_t *emu, decoded_inst_t *inst);

/* 自测: 算术 / 比较 / 转换 / 标志位 / CSR */
void fpu_selftest(void);

#endif /* _RVEMU_FPU_H */
