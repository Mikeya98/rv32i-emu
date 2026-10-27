/**
 * @file    trap.h
 * @brief   RV32I Emulator — trap 框架 (异常 + 中断 + mret)
 *
 * RISC-V 把异常和中断统一叫 trap:
 *
 *   异常 (exception) 同步发生, 由当前指令引起 —— 非法指令、访存错误、
 *        ecall。mcause 最高位为 0。
 *   中断 (interrupt) 异步发生, 在指令之间检查 —— 定时器到期等。
 *        mcause 最高位为 1, 低位是中断号。
 *
 * 两者进入 handler 的流程一样: 硬件保存现场 → 跳到 mtvec → 软件处理
 * → mret 返回。这个模拟器里"硬件"就是 trap_raise()。
 */

#ifndef _RVEMU_TRAP_H
#define _RVEMU_TRAP_H

#include "types.h"
#include "emu.h"

/** 初始化 trap 相关状态 (mstatus 复位值等) */
void trap_init(emulator_t *emu);

/**
 * @brief 进入 trap
 *
 * 做四件事:
 *   1. mepc = 出错指令地址 (中断时是下一条待执行指令)
 *   2. mcause = 原因, mtval = 附加信息
 *   3. mstatus: MPIE ← MIE, MIE ← 0, MPP ← M
 *   4. pc ← mtvec
 *
 * @param  epc  保存到 mepc 的地址
 * @return true = 已进入 handler; false = 没有安装 handler (mtvec == 0)
 */
bool trap_raise(emulator_t *emu, u32 cause, u32 tval, u32 epc);

/**
 * @brief 每条指令执行前调用: 推进 mtime, 检查定时器中断
 * @return true = 触发了中断并跳进了 handler
 */
bool trap_check_interrupt(emulator_t *emu, u32 epc);

/**
 * @brief mret: 从 trap handler 返回
 * @return 应该跳转到的 pc (mepc)
 */
u32 trap_mret(emulator_t *emu);

/** 自测 */
void trap_selftest(void);

#endif /* _RVEMU_TRAP_H */
