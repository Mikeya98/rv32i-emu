/**
 * @file    csr.h
 * @brief   RV32I Emulator — CSR (Control & Status Registers)
 *
 * 最小特权架构:
 *   - mtvec    (0x305): trap 向量基址
 *   - mepc     (0x341): 异常返回地址
 *   - mcause   (0x342): 异常原因
 *   - mstatus  (0x300): 机器状态 (MIE bit)
 *   - mie      (0x304): 中断使能
 *   - mip      (0x344): 中断 pending
 *   - mcycle   (0xB00): 周期计数器
 */

#ifndef _RVEMU_CSR_H
#define _RVEMU_CSR_H

#include "types.h"

/* CSR 地址 */
#define CSR_MSTATUS     0x300
#define CSR_MTVEC       0x305
#define CSR_MEPC        0x341
#define CSR_MCAUSE      0x342
#define CSR_MIE         0x304
#define CSR_MIP         0x344
#define CSR_MCYCLE      0xB00

#define CSR_MMIO_BASE   0x20000000  /* 简易 MMIO 基址 */
#define CSR_MMIO_UART   0x20000000  /* UART 输出 (写字节即打印) */

/* mcause 值 */
#define MCAUSE_ECALL    11   /* Environment call from M-mode */

typedef struct {
    u32  mstatus;
    u32  mtvec;
    u32  mepc;
    u32  mcause;
    u32  mie;
    u32  mip;
    u64  mcycle;       /* 64-bit wide */
} csr_t;

/* --- API --- */

void  csr_init(csr_t *csr);
u32   csr_read (csr_t *csr, u32 addr, bool *ok);
void  csr_write(csr_t *csr, u32 addr, u32 val, bool *ok);
void  csr_dump (csr_t *csr);

#endif /* _RVEMU_CSR_H */
