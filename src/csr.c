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

void csr_dump(csr_t *csr)
{
    printf("CSR:\n");
    printf("  mstatus  = 0x%08X  (MIE=%u MPIE=%u MPP=%u)\n",
           csr->mstatus,
           (csr->mstatus & MSTATUS_MIE) ? 1u : 0u,
           (csr->mstatus & MSTATUS_MPIE) ? 1u : 0u,
           (csr->mstatus & MSTATUS_MPP_MASK) >> 11);
    printf("  mtvec    = 0x%08X\n", csr->mtvec);
    printf("  mepc     = 0x%08X\n", csr->mepc);
    printf("  mcause   = 0x%08X%s\n", csr->mcause,
           (csr->mcause & MCAUSE_IRQ_BIT) ? "  (interrupt)" : "  (exception)");
    printf("  mtval    = 0x%08X\n", csr->mtval);
    printf("  mie/mip  = 0x%08X / 0x%08X\n", csr->mie, csr->mip);
    printf("  mcycle   = %llu\n",   (unsigned long long)csr->mcycle);
}
