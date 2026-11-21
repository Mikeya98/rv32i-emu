/**
 * @file    plic.h
 * @brief   RV32I Emulator — PLIC (Platform-Level Interrupt Controller) (v0.14)
 *
 * PLIC 是 RISC-V 平台上所有外部设备共用的中断控制器。设备把中断线
 * 拉到 PLIC 上, PLIC 按优先级、使能位和阈值决定要不要通知某个核:
 *
 *     设备 IRQ 线 ──▶ pending ──▶ & enable[ctx] ──▶ 优先级 > threshold[ctx]
 *                                                        │
 *                                                        ▼
 *                                                mip.MEIP ──▶ 该核进 trap
 *
 * 软件的处理流程是标准的 claim/complete:
 *   claim  : 读 claim 寄存器, 拿到最高优先级的待处理 IRQ (同时清 pending)
 *   complete: 处理完把 IRQ 号写回 claim 寄存器, 告诉 PLIC 可以再报了
 *
 * 地址布局沿用 QEMU virt / SiFive 的约定, 方便和真实平台对照。
 *
 * 本模拟器的简化:
 *   - 32 个中断源 (IRQ 1..31), 每个上下文一个 32 位 enable 字
 *   - 每个 hart 两个上下文 (v0.15): M 态一个、S 态一个, 编号和
 *     QEMU virt 一致 —— hart h 的 M 上下文是 2h, S 上下文是 2h+1。
 *     M 上下文拉 mip.MEIP, S 上下文拉 mip.SEIP。
 *   - 中断源按边沿语义: 设备触发一次置 pending, claim 后不会自动重报
 *     (真实电平设备会在 complete 后重新拉起, 需要设备自己配合)
 */

#ifndef _RVEMU_PLIC_H
#define _RVEMU_PLIC_H

#include "types.h"

#define PLIC_BASE            0x0C000000u
#define PLIC_MAX_IRQ         32          /* 含保留的 IRQ 0 */
#define PLIC_MAX_CTX         8           /* 每个 hart 两个上下文 (M/S) */

/*
 * 上下文编号 (v0.15): 和设备树里 QEMU virt 的写法一样,
 *   hart h 的 M 态上下文 = 2h, S 态上下文 = 2h+1
 */
#define PLIC_CTX_M(hart)     (2u * (hart))
#define PLIC_CTX_S(hart)     (2u * (hart) + 1u)

/* 寄存器偏移 (和 QEMU virt 一致) */
#define PLIC_PRIORITY(irq)   (PLIC_BASE + 4u * (irq))
#define PLIC_PENDING_BASE    (PLIC_BASE + 0x1000u)
#define PLIC_ENABLE_BASE     (PLIC_BASE + 0x2000u)
#define PLIC_ENABLE_STRIDE   0x80u
#define PLIC_CONTEXT_BASE    (PLIC_BASE + 0x200000u)
#define PLIC_CONTEXT_STRIDE  0x1000u
#define PLIC_THRESHOLD(ctx)  (PLIC_CONTEXT_BASE + PLIC_CONTEXT_STRIDE * (ctx))
#define PLIC_CLAIM(ctx)      (PLIC_THRESHOLD(ctx) + 4u)

/*
 * 虚拟设备: 一个"门铃"寄存器。
 * 真实平台上是 UART、网卡这些设备各自拉中断线; 模拟器里没有设备,
 * 所以放一个最简单的: 往 0x10000000 写字, bit n = 1 表示设备 n
 * 触发了一次中断。self-test 也可以直接调 plic_raise()。
 */
#define PLIC_DEV_BASE        0x10000000u
#define PLIC_DEV_DOORBELL    (PLIC_DEV_BASE + 0x00u)

typedef struct {
    u32 priority[PLIC_MAX_IRQ];   /* priority[0] 保留, 写 0 表示关闭 */
    u32 pending;                  /* bit i = IRQ i 等待处理 */
    u32 enable[PLIC_MAX_CTX];     /* 每个上下文一个使能字 */
    u32 threshold[PLIC_MAX_CTX];  /* 优先级必须严格大于它才通知 */
    u32 in_service[PLIC_MAX_CTX]; /* claim 之后、complete 之前 */
} plic_t;

/* --- API --- */

void plic_init(plic_t *p);

/** 设备拉高一条中断线 (边沿触发) */
void plic_raise(plic_t *p, u32 irq);

/** 撤掉一条中断线 (调试/电平设备用) */
void plic_clear(plic_t *p, u32 irq);

/** 该上下文此刻是否有一条够格通知 CPU 的中断 */
bool plic_pending_for(const plic_t *p, u32 ctx);

/** claim: 返回最高优先级的 IRQ 并清 pending, 没有则返回 0 */
u32  plic_claim(plic_t *p, u32 ctx);

/** complete: 告诉 PLIC 这个 IRQ 处理完了 */
void plic_complete(plic_t *p, u32 ctx, u32 irq);

/** MMIO 访问: 命中返回 true 并写 *val */
bool plic_mmio_read(const plic_t *p, u32 pa, u32 *val);
bool plic_mmio_write(plic_t *p, u32 pa, u32 val);

/** 虚拟设备门铃: 写 1 到 bit n 等于设备 n 触发中断 */
bool plic_device_write(plic_t *p, u32 pa, u32 val);

/** PLIC 自测 (v0.14) */
void plic_selftest(void);

/** S 态 PLIC 上下文 + SEI + WFI 自测 (v0.15) */
void plic_smode_selftest(void);

#endif /* _RVEMU_PLIC_H */
