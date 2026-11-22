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

/*
 * v0.12: trap 状态全部下沉到 hart_t, 由 emu_init_harts() 统一复位:
 *   - mstatus 复位成 FS = Initial
 *   - mtimecmp = 0 (定时器关闭)
 *   - mtime 是全机器共享的, 放在 emulator_t 里
 */

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
 * @brief 当前 hart 是否有已使能的 pending 中断 (v0.15, 供 WFI 使用)
 *
 * 只看 pending & enable, 不看 mstatus.MIE/SIE —— WFI 的唤醒条件
 * 和"能不能立刻进 trap"是两回事。
 */
bool trap_irq_pending(emulator_t *emu);

/**
 * @brief mret: 从 trap handler 返回
 * @return 应该跳转到的 pc (mepc)
 */
u32 trap_mret(emulator_t *emu);

/**
 * @brief sret: 从 S 态 trap handler 返回 (v0.5)
 * @return 应该跳转到的 pc (sepc)
 */
u32 trap_sret(emulator_t *emu);

/** 自测 */
void trap_selftest(void);
void trap_smode_selftest(void);
void trap_stimer_selftest(void);   /* v0.16: STIP + Sstc stimecmp */

#endif /* _RVEMU_TRAP_H */
