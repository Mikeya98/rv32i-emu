/**
 * @file    csr.c
 * @brief   RV32I Emulator — CSR 实现
 */

#include "csr.h"
#include <stdio.h>
#include <string.h>

void csr_init(csr_t *csr)
{
    memset(csr, 0, sizeof(*csr));
}

u32 csr_read(csr_t *csr, u32 addr, bool *ok)
{
    if (ok) *ok = true;

    switch (addr) {
    case CSR_MSTATUS:  return csr->mstatus;
    case CSR_SSTATUS:  return csr->mstatus & SSTATUS_MASK;
    case CSR_SATP:     return csr->satp;
    case CSR_SIE:      return csr->sie;
    case CSR_STVEC:    return csr->stvec;
    case CSR_SSCRATCH: return csr->sscratch;
    case CSR_SEPC:     return csr->sepc;
    case CSR_SCAUSE:   return csr->scause;
    case CSR_STVAL:    return csr->stval;
    case CSR_SIP:      return csr->sip;
    case CSR_MEDELEG:  return csr->medeleg;
    case CSR_MIDELEG:  return csr->mideleg;
    case CSR_MIE:      return csr->mie;
    case CSR_MTVEC:    return csr->mtvec;
    case CSR_MSCRATCH: return csr->mscratch;
    case CSR_MEPC:     return csr->mepc;
    case CSR_MCAUSE:   return csr->mcause;
    case CSR_MTVAL:    return csr->mtval;
    case CSR_MIP:      return csr->mip;
    case CSR_MCYCLE:   return (u32)(csr->mcycle & 0xFFFFFFFF);
    default:
        if (ok) *ok = false;
        return 0;
    }
}

void csr_write(csr_t *csr, u32 addr, u32 val, bool *ok)
{
    if (ok) *ok = true;

    switch (addr) {
    case CSR_MSTATUS:  csr->mstatus  = val; break;
    /* sstatus 只能改 mstatus 里属于 S 态的那几位 */
    case CSR_SSTATUS:
        csr->mstatus = (csr->mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK);
        break;
    case CSR_SIE:      csr->sie      = val; break;
    case CSR_STVEC:    csr->stvec    = val & ~0x3u; break;
    case CSR_SSCRATCH: csr->sscratch = val; break;
    case CSR_SEPC:     csr->sepc     = val & ~0x3u; break;
    case CSR_SCAUSE:   csr->scause   = val; break;
    case CSR_STVAL:    csr->stval    = val; break;
    case CSR_SIP:      csr->sip     &= val; break;   /* 软件只能清 pending */
    case CSR_MEDELEG:  csr->medeleg  = val & 0xFFFFu; break;
    case CSR_MIDELEG:  csr->mideleg  = val & 0xFFFFu; break;
    /*
     * satp (v0.4): 只实现 Sv32。MODE 只接受 0 (bare) 和 1 (Sv32),
     * 别的值按规范是 WARL, 这里选择保持原值不变 (拒绝写入)。
     */
    case CSR_SATP:
        if ((val & SATP_MODE_BIT) == 0) {
            csr->satp = 0;                  /* MODE = 0: bare */
        } else {
            /* MODE = 1: Sv32, 保留 ASID[30:22] 和 PPN[21:0] */
            csr->satp = val & (SATP_MODE_BIT | (0x1FFu << 22) | SATP_PPN_MASK);
        }
        break;
    case CSR_MIE:      csr->mie      = val; break;
    case CSR_MTVEC:    csr->mtvec    = val & ~0x3u; break;  /* 低两位是模式 */
    case CSR_MSCRATCH: csr->mscratch = val; break;
    case CSR_MEPC:     csr->mepc     = val & ~0x3u; break;  /* 4 字节对齐 */
    case CSR_MCAUSE:   csr->mcause   = val; break;
    case CSR_MTVAL:    csr->mtval    = val; break;
    case CSR_MIP:      csr->mip     &= val; break;   /* 软件只能清 pending 位 */
    case CSR_MCYCLE:   csr->mcycle   = (csr->mcycle & 0xFFFFFFFF00000000ULL) | val; break;
    default:
        if (ok) *ok = false;
        break;
    }
}

bool csr_priv_ok(u32 priv, u32 addr)
{
    /* CSR 地址 [9:8]: 00=U, 01=S, 11=M; 未实现的 10 (H) 按 M 处理 */
    u32 need = (addr >> 8) & 0x3u;
    if (need == 2u) {
        need = 3u;
    }
    return priv >= need;
}

bool csr_writable(u32 addr)
{
    return ((addr >> 10) & 0x3u) != 0x3u;
}

void csr_dump(csr_t *csr)
{
    printf("CSR:\n");
    printf("  mstatus  = 0x%08X  (MIE=%u MPIE=%u MPP=%u)\n",
           csr->mstatus,
           (csr->mstatus & MSTATUS_MIE) ? 1u : 0u,
           (csr->mstatus & MSTATUS_MPIE) ? 1u : 0u,
           (csr->mstatus & MSTATUS_MPP_MASK) >> 11);
    printf("  satp     = 0x%08X  (mode=%s root=0x%X)\n",
           csr->satp,
           (csr->satp & 0x80000000u) ? "Sv32" : "bare",
           (csr->satp & 0x003FFFFFu) << 12);
    printf("  mtvec    = 0x%08X\n", csr->mtvec);
    printf("  mepc     = 0x%08X\n", csr->mepc);
    printf("  mcause   = 0x%08X%s\n", csr->mcause,
           (csr->mcause & MCAUSE_IRQ_BIT) ? "  (interrupt)" : "  (exception)");
    printf("  mtval    = 0x%08X\n", csr->mtval);
    printf("  mie/mip  = 0x%08X / 0x%08X\n", csr->mie, csr->mip);
    printf("  mcycle   = %llu\n",   (unsigned long long)csr->mcycle);
    printf("  S-mode: sstatus=0x%08X (SIE=%u SPIE=%u SPP=%u SUM=%u)\n",
           csr->mstatus & SSTATUS_MASK,
           (csr->mstatus & MSTATUS_SIE) ? 1u : 0u,
           (csr->mstatus & MSTATUS_SPIE) ? 1u : 0u,
           (csr->mstatus & MSTATUS_SPP) ? 1u : 0u,
           (csr->mstatus & MSTATUS_SUM) ? 1u : 0u);
    printf("  stvec    = 0x%08X  sepc = 0x%08X  scause = 0x%08X  stval = 0x%08X\n",
           csr->stvec, csr->sepc, csr->scause, csr->stval);
    printf("  medeleg  = 0x%08X  mideleg = 0x%08X\n",
           csr->medeleg, csr->mideleg);
}
