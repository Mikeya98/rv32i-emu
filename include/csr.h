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
#define CSR_FFLAGS      0x001   /* 浮点异常标志 (v0.8 RV32F) */
#define CSR_FRM         0x002   /* 浮点舍入模式 */
#define CSR_FCSR        0x003   /* frm[7:5] | fflags[4:0] */
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
#define CSR_MHARTID     0xF14   /* 本 hart 的编号 (只读, v0.12) */
#define CSR_MCYCLE      0xB00

/* satp 字段 (Sv32) */
#define SATP_MODE_BIT    (1u << 31)     /* 1 = Sv32, 0 = bare */
#define SATP_PPN_MASK    0x003FFFFFu

#define CSR_MMIO_BASE   0x20000000  /* 简易 MMIO 基址 */
#define CSR_MMIO_UART   0x20000000  /* UART 输出 (写字节即打印) */

/*
 * CLINT 简化版: 地址与真实 CLINT 一致。
 *   msip     每个 hart 一个 32 位寄存器, 只有 bit0 有意义:
 *            hart0 在 0x02000000, hart1 在 0x02000004 … (v0.13)
 *            写 1 拉起该核的软件中断, 写 0 拉低。电平信号, 不清就一直挂。
 *   mtimecmp 每个 hart 占 8 字节: hart0 在 0x02004000, hart1 在 0x02004008 …
 *   mtime 全机器共享一个 64 位计数器, 低 32 位在 0x0200BFF8。
 * 只实现低 32 位比较 (高 32 位写入被忽略)。
 */
#define CLINT_MSIP_BASE     0x02000000u
#define CLINT_MSIP(h)       (CLINT_MSIP_BASE + 4u * (h))
#define CLINT_MSIP_END      (CLINT_MSIP_BASE + 4u * 4u)   /* 最多 MAX_HARTS 个 */
#define CLINT_MTIMECMP_BASE 0x02004000u
#define CLINT_MTIMECMP_LO(h) (CLINT_MTIMECMP_BASE + 8u * (h))
#define CLINT_MTIME_LO      0x0200BFF8u
#define CLINT_MTIME_HI      0x0200BFFCu

/*
 * PLIC (v0.14): 外部中断控制器, 基址和寄存器布局见 plic.h。
 *   0x0C000000 + 4*irq       优先级
 *   0x0C001000               pending
 *   0x0C002000 + 0x80*ctx    每个上下文的使能字
 *   0x0C200000 + 0x1000*ctx  threshold(+0) / claim-complete(+4)
 * v0.15: 每个 hart 两个上下文, M = 2h, S = 2h+1。
 */

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

/*
 * mstatus.FS[14:13] (v0.11): 浮点单元状态
 *   00 Off     —— 浮点关闭, 任何 FP 指令 / fcsr 访问都算非法指令
 *   01 Initial —— 刚开启, 还没执行过 FP 指令
 *   10 Clean   —— 执行过, 但上次 trap 之后保存过现场 (本模拟器不主动用)
 *   11 Dirty   —— 执行过 FP 指令, 现场需要保存
 *
 * sstatus 是 mstatus 的受限视图, FS 两位在 S 态也可见/可写。
 */
#define MSTATUS_FS_SHIFT   13
#define MSTATUS_FS_MASK    (3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_OFF     (0u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL (1u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_CLEAN   (2u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_DIRTY   (3u << MSTATUS_FS_SHIFT)

/* sstatus 是 mstatus 的一个受限视图 */
#define SSTATUS_MASK       (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | \
                            MSTATUS_SUM | MSTATUS_FS_MASK)

/* 特权级编码 (和 RISC-V 规范一致) */
#define PRIV_U             0u
#define PRIV_S             1u
#define PRIV_M             3u

/* mie / mip 位 */
#define MIE_MSIE           (1u << 3)    /* 机器软件中断使能 (v0.13) */
#define MIP_MSIP           (1u << 3)    /* 机器软件中断 pending */
#define MIE_MTIE           (1u << 7)    /* 机器定时器中断使能 */
#define MIP_MTIP           (1u << 7)    /* 机器定时器中断 pending */
#define MIE_MEIE           (1u << 11)   /* 机器外部中断使能 (v0.14) */
#define MIP_MEIP           (1u << 11)   /* 机器外部中断 pending */
#define MIE_SEIE           (1u << 9)    /* S 态外部中断使能 (v0.15) */
#define MIP_SEIP           (1u << 9)    /* S 态外部中断 pending */

/* mcause: 最高位 = 1 表示中断, 否则是异常 */
#define MCAUSE_IRQ_BIT     0x80000000u
#define MCAUSE_IRQ_SOFT    (MCAUSE_IRQ_BIT | 3)   /* 3 = machine software (v0.13) */
#define MCAUSE_IRQ_TIMER   (MCAUSE_IRQ_BIT | 7)   /* 7 = machine timer */
#define MCAUSE_IRQ_EXT     (MCAUSE_IRQ_BIT | 11)  /* 11 = machine external (v0.14) */
#define MCAUSE_IRQ_S_EXT   (MCAUSE_IRQ_BIT | 9)   /* 9 = supervisor external (v0.15) */

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

    /* 浮点 (v0.8): fcsr = frm << 5 | fflags */
    u32  fflags;       /* NV=4 DZ=3 OF=2 UF=1 NX=0 */
    u32  frm;          /* 0=RNE 1=RTZ 2=RDN 3=RUP 4=RMM 7=动态 */

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

    /* v0.12: 本 hart 编号, 复位时写入, 之后只读 */
    u32  mhartid;
} csr_t;

/* --- API --- */

void  csr_init(csr_t *csr);
u32   csr_read (csr_t *csr, u32 addr, bool *ok);
void  csr_write(csr_t *csr, u32 addr, u32 val, bool *ok);
void  csr_dump (csr_t *csr);

/** CSR 地址 [9:8] 编码了最低可访问特权级 (U=0, S=1, M=3) */
bool  csr_priv_ok(u32 priv, u32 addr);

/** FS != Off: 浮点单元可用 (v0.11) */
static inline bool csr_fpu_enabled(const csr_t *csr)
{
    return (csr->mstatus & MSTATUS_FS_MASK) != 0u;
}

/**
 * @brief 执行了一条 FP 指令 / 写了一次浮点 CSR
 *
 * 按规范, FS 从 Initial/Clean 变成 Dirty; Off 状态下根本不该走到这里
 * (调用者应先判非法指令), 这里再挡一道。
 */
static inline void csr_fpu_dirty(csr_t *csr)
{
    if (csr_fpu_enabled(csr)) {
        csr->mstatus |= MSTATUS_FS_DIRTY;
    }
}

/** CSR 地址 [11:10] == 11 表示只读 */
bool  csr_writable(u32 addr);

#endif /* _RVEMU_CSR_H */
