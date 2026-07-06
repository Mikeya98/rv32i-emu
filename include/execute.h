/**
 * @file    execute.h
 * @brief   RV32I Emulator — 执行引擎
 *
 * 接收解码后的指令，对寄存器/内存/CSR 执行操作。
 * 覆盖 RV32I Base Integer ISA 全部 40 条指令。
 */

#ifndef _RVEMU_EXECUTE_H
#define _RVEMU_EXECUTE_H

#include "types.h"
#include "emu.h"

/**
 * @brief  执行一条解码后的指令
 * @param  emu   模拟器状态 (memory + regfile + csr + pc + state)
 * @param  inst  解码后的指令
 * @return true=正常, false=异常/未知指令
 */
bool execute(emulator_t *emu, decoded_inst_t *inst);

/* 自测: 覆盖 ALU / 分支 / 跳转 / 访存 */
void execute_selftest(void);

#endif /* _RVEMU_EXECUTE_H */
