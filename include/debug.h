/**
 * @file    debug.h
 * @brief   RV32I Emulator — 交互式调试器
 *
 * 命令: 单步 / 继续 / 断点 / 寄存器 / 内存 / 反汇编
 */

#ifndef _RVEMU_DEBUG_H
#define _RVEMU_DEBUG_H

#include "emu.h"

/**
 * @brief  进入交互式调试循环 (stdin 读命令, 直到 quit 或 EOF)
 */
void debug_repl(emulator_t *emu);

#endif /* _RVEMU_DEBUG_H */
