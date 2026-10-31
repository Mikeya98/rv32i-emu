/**
 * @file    csr.h
 * @brief   RV32I Emulator — CSR (Control & Status Registers)
 *
 * 最小特权架构 (v0.3 起支持 trap):
 *   - mstatus  (0x300): 机器状态 (MIE/MPIE/MPP)
 *   - mie      (0x304): 中断使能
 *   - mtvec    (0x305): trap 向量基址
 *   - mscratch (0x340): 机器模式暂存寄存器
 *   - mepc     (0x341): trap 返回地址
 *   - mcause   (0x342): trap 原因
 *   - mtval    (0x343): trap 附加信息 (故障地址 / 指令)
 *   - mip      (0x344): 中断 pending
 *   - mcycle   (0xB00): 周期计数器
 *   - satp     (0x180): 地址空间标识 + Sv32 页表根 (v0.4)
 */

#ifndef _RVEMU_CSR_H
#define _RVEMU_CSR_H

#include "types.h"

/* CSR 地址 */
#define CSR_SSTATUS     0x100
#define CSR_SIE         0x104
#define CSR_STVEC       0x105
#define CSR_SSCRATCH    0x140
#define CSR_SEPC        0x141
#define CSR_SCAUSE      0x142
#define CSR_STVAL       0x143
#define CSR_SIP         0x144
#define CSR_SATP        0x180
#define CSR_MSTATUS     0x300
#define CSR_MEDELEG     0x302
#define CSR_MIDELEG     0x303
#define CSR_MIE         0x304
#define CSR_MTVEC       0x305
#define CSR_MSCRATCH    0x340
#define CSR_MEPC        0x341
#define CSR_MCAUSE      0x342
#define CSR_MTVAL       0x343
#define CSR_MIP         0x344
#define CSR_MCYCLE      0xB00

/* satp 字段 (Sv32) */
#define SATP_MODE_BIT    (1u << 31)     /* 1 = Sv32, 0 = bare */
#define SATP_PPN_MASK    0x003FFFFFu

#define CSR_MMIO_BASE   0x20000000  /* 简易 MMIO 基址 */
#define CSR_MMIO_UART   0x20000000  /* UART 输出 (写字节即打印) */

/* CLINT 简化版: 只实现 mtime / mtimecmp 的低 32 位 (地址与真实 CLINT 一致) */
#define CLINT_MTIMECMP_LO  0x02004000u
#define CLINT_MTIME_LO     0x0200BFF8u

/* mstatus 位 */
#define MSTATUS_SIE        (1u << 1)    /* S 态中断使能 */
#define MSTATUS_MIE        (1u << 3)    /* 全局中断使能 */
#define MSTATUS_SPIE       (1u << 5)    /* S trap 前的 SIE 备份 */
#define MSTATUS_MPIE       (1u << 7)    /* M trap 前的 MIE 备份 */
#define MSTATUS_SPP        (1u << 8)    /* S trap 前的特权级 (1=S, 0=U) */
#define MSTATUS_MPP_MASK   (3u << 11)   /* 特权级备份 */
#define MSTATUS_MPP_M      (3u << 11)   /* 11 = M-mode */
#define MSTATUS_MPP_S      (1u << 11)   /* 01 = S-mode (v0.5) */
#define MSTATUS_MPP_U      (0u << 11)   /* 00 = U-mode (v0.4) */
#define MSTATUS_SUM        (1u << 18)   /* S 态允许访问 U 页 */
#define MSTATUS_MPRV       (1u << 17)   /* 未实现, 保留定义 */

/* sstatus 是 mstatus 的一个受限视图 */
#define SSTATUS_MASK       (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | MSTATUS_SUM)

/* 特权级编码 (和 RISC-V 规范一致) */
#define PRIV_U             0u
#define PRIV_S             1u
#define PRIV_M             3u

/* mie / mip 位 */
#define MIE_MTIE           (1u << 7)    /* 机器定时器中断使能 */
#define MIP_MTIP           (1u << 7)    /* 机器定时器中断 pending */

/* mcause: 最高位 = 1 表示中断, 否则是异常 */
#define MCAUSE_IRQ_BIT     0x80000000u
#define MCAUSE_IRQ_TIMER   (MCAUSE_IRQ_BIT | 7)   /* 7 = machine timer */

/* 异常码 (和 RISC-V 规范一致) */
#define EXC_INST_MISALIGNED  0
#define EXC_INST_ACCESS      1
#define EXC_ILLEGAL_INST     2
#define EXC_BREAKPOINT       3
#define EXC_LOAD_MISALIGNED  4
#define EXC_LOAD_ACCESS      5
#define EXC_STORE_MISALIGNED 6
#define EXC_STORE_ACCESS     7
#define EXC_ECALL_U          8
#define EXC_ECALL_S          9
#define EXC_ECALL_M          11
#define EXC_INST_PAGE_FAULT  12
#define EXC_LOAD_PAGE_FAULT  13
#define EXC_STORE_PAGE_FAULT 15

typedef struct {
    u32  mstatus;
    u32  satp;         /* Sv32: MODE[31] | ASID[30:22] | PPN[21:0] */

    /* S 态 (v0.5) */
    u32  stvec;
    u32  sepc;
    u32  scause;
    u32  stval;
    u32  sscratch;
    u32  sie;
    u32  sip;

    /* 委托: 当前级 <= S 时, 对应位为 1 的 trap 交给 S 态处理 */
    u32  medeleg;
    u32  mideleg;

    u32  mtvec;
    u32  mepc;
    u32  mcause;
    u32  mtval;
    u32  mscratch;
    u32  mie;
    u32  mip;
    u64  mcycle;       /* 64-bit wide */
} csr_t;

/* --- API --- */

void  csr_init(csr_t *csr);
u32   csr_read (csr_t *csr, u32 addr, bool *ok);
void  csr_write(csr_t *csr, u32 addr, u32 val, bool *ok);
void  csr_dump (csr_t *csr);

/** CSR 地址 [9:8] 编码了最低可访问特权级 (U=0, S=1, M=3) */
bool  csr_priv_ok(u32 priv, u32 addr);

/** CSR 地址 [11:10] == 11 表示只读 */
bool  csr_writable(u32 addr);

#endif /* _RVEMU_CSR_H */
