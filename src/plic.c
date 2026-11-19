/**
 * @file    plic.c
 * @brief   RV32I Emulator — PLIC 实现与自测 (v0.14)
 */

#include "plic.h"
#include "emu.h"
#include "csr.h"
#include "trap.h"
#include "memory.h"
#include <stdio.h>

/* =========================== 基础操作 =========================== */

void plic_init(plic_t *p)
{
    u32 i;

    for (i = 0; i < PLIC_MAX_IRQ; i++) {
        p->priority[i] = 0;
    }
    p->pending = 0;

    for (i = 0; i < PLIC_MAX_CTX; i++) {
        p->enable[i]     = 0;
        p->threshold[i]  = 0;
        p->in_service[i] = 0;
    }
}

void plic_raise(plic_t *p, u32 irq)
{
    if (irq == 0 || irq >= PLIC_MAX_IRQ) {
        return;                 /* IRQ 0 保留, 永远不能 pending */
    }
    p->pending |= (1u << irq);
}

void plic_clear(plic_t *p, u32 irq)
{
    if (irq == 0 || irq >= PLIC_MAX_IRQ) {
        return;
    }
    p->pending &= ~(1u << irq);
}

bool plic_pending_for(const plic_t *p, u32 ctx)
{
    u32 avail;
    u32 irq;

    if (ctx >= PLIC_MAX_CTX) {
        return false;
    }

    avail = p->pending & p->enable[ctx] & 0xFFFFFFFEu;

    for (irq = 1; irq < PLIC_MAX_IRQ; irq++) {
        if ((avail & (1u << irq)) &&
            p->priority[irq] > p->threshold[ctx]) {
            return true;
        }
    }
    return false;
}

u32 plic_claim(plic_t *p, u32 ctx)
{
    u32 best = 0;
    u32 best_prio = 0;
    u32 avail;
    u32 irq;

    if (ctx >= PLIC_MAX_CTX) {
        return 0;
    }

    avail = p->pending & p->enable[ctx] & 0xFFFFFFFEu;

    /*
     * 优先级最高的先报; 优先级相同时编号小的先报。
     * 用严格大于比较, 遍历从 1 开始, 自然就得到"同优先级取小号"。
     */
    for (irq = 1; irq < PLIC_MAX_IRQ; irq++) {
        if (!(avail & (1u << irq))) {
            continue;
        }
        if (p->priority[irq] <= p->threshold[ctx]) {
            continue;
        }
        if (p->priority[irq] > best_prio) {
            best_prio = p->priority[irq];
            best = irq;
        }
    }

    if (best != 0) {
        p->pending    &= ~(1u << best);
        p->in_service[ctx] |= (1u << best);
    }
    return best;
}

void plic_complete(plic_t *p, u32 ctx, u32 irq)
{
    if (ctx >= PLIC_MAX_CTX || irq == 0 || irq >= PLIC_MAX_IRQ) {
        return;
    }
    p->in_service[ctx] &= ~(1u << irq);
}

/* =========================== MMIO =========================== */

bool plic_mmio_read(const plic_t *p, u32 pa, u32 *val)
{
    u32 off;

    /* 优先级: 0x0C000000 + 4*irq */
    if (pa >= PLIC_BASE && pa < PLIC_PENDING_BASE) {
        u32 irq = (pa - PLIC_BASE) / 4u;
        *val = (irq < PLIC_MAX_IRQ) ? p->priority[irq] : 0u;
        return true;
    }

    /* pending: 0x0C001000, 一个 32 位字覆盖 IRQ 0..31 */
    if (pa >= PLIC_PENDING_BASE && pa < PLIC_PENDING_BASE + 8u) {
        off = pa - PLIC_PENDING_BASE;
        *val = (off < 4u) ? p->pending : 0u;
        return true;
    }

    /* enable: 每个上下文 0x80 字节, 我们只实现第一个字 */
    if (pa >= PLIC_ENABLE_BASE &&
        pa < PLIC_ENABLE_BASE + PLIC_ENABLE_STRIDE * PLIC_MAX_CTX) {
        u32 ctx = (pa - PLIC_ENABLE_BASE) / PLIC_ENABLE_STRIDE;
        off = (pa - PLIC_ENABLE_BASE) % PLIC_ENABLE_STRIDE;
        *val = (off < 4u) ? p->enable[ctx] : 0u;
        return true;
    }

    /* 上下文区: threshold 在 +0, claim/complete 在 +4 */
    if (pa >= PLIC_CONTEXT_BASE &&
        pa < PLIC_CONTEXT_BASE + PLIC_CONTEXT_STRIDE * PLIC_MAX_CTX) {
        u32 ctx = (pa - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_STRIDE;
        off = (pa - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_STRIDE;

        if (off == 0u) {
            *val = p->threshold[ctx];
            return true;
        }
        if (off == 4u) {
            /* 读 claim = 领走一条中断; 没有就返回 0 */
            *val = plic_claim((plic_t *)p, ctx);
            return true;
        }
        *val = 0u;
        return true;
    }

    return false;
}

bool plic_mmio_write(plic_t *p, u32 pa, u32 val)
{
    u32 off;

    if (pa >= PLIC_BASE && pa < PLIC_PENDING_BASE) {
        u32 irq = (pa - PLIC_BASE) / 4u;
        if (irq == 0u) {
            return true;        /* IRQ 0 保留: 丢弃 */
        }
        if (irq < PLIC_MAX_IRQ) {
            p->priority[irq] = val;
        }
        return true;
    }

    /* pending 是只读的: 只有设备能拉起它 */
    if (pa >= PLIC_PENDING_BASE && pa < PLIC_PENDING_BASE + 8u) {
        return true;
    }

    if (pa >= PLIC_ENABLE_BASE &&
        pa < PLIC_ENABLE_BASE + PLIC_ENABLE_STRIDE * PLIC_MAX_CTX) {
        u32 ctx = (pa - PLIC_ENABLE_BASE) / PLIC_ENABLE_STRIDE;
        off = (pa - PLIC_ENABLE_BASE) % PLIC_ENABLE_STRIDE;
        if (off < 4u) {
            /* bit0 对应 IRQ 0, 保留 */
            p->enable[ctx] = val & 0xFFFFFFFEu;
        }
        return true;
    }

    if (pa >= PLIC_CONTEXT_BASE &&
        pa < PLIC_CONTEXT_BASE + PLIC_CONTEXT_STRIDE * PLIC_MAX_CTX) {
        u32 ctx = (pa - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_STRIDE;
        off = (pa - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_STRIDE;

        if (off == 0u) {
            p->threshold[ctx] = val;
            return true;
        }
        if (off == 4u) {
            /* 写 claim = complete */
            plic_complete(p, ctx, val);
            return true;
        }
        return true;
    }

    return false;
}

bool plic_device_write(plic_t *p, u32 pa, u32 val)
{
    u32 irq;

    if (pa != PLIC_DEV_DOORBELL) {
        return false;
    }

    /* 门铃: 一次写可以同时触发多条中断线 */
    for (irq = 1; irq < PLIC_MAX_IRQ; irq++) {
        if (val & (1u << irq)) {
            plic_raise(p, irq);
        }
    }
    return true;
}

/* =========================== 自测 =========================== */

static u32 enc_i(s32 imm, u32 rs1, u32 f3, u32 rd, u32 op)
{
    return ((u32)imm & 0xFFFu) << 20 | (rs1 & 0x1Fu) << 15 |
           (f3 & 7u) << 12 | (rd & 0x1Fu) << 7 | (op & 0x7Fu);
}

static u32 enc_s(s32 imm, u32 rs2, u32 rs1, u32 f3)
{
    u32 u = (u32)imm;
    return ((u >> 5) & 0x7Fu) << 25 | (rs2 & 0x1Fu) << 20 |
           (rs1 & 0x1Fu) << 15 | (f3 & 7u) << 12 |
           (u & 0x1Fu) << 7 | 0x23u;
}

static u32 enc_lui(u32 imm20, u32 rd)
{
    return (imm20 & 0xFFFFFu) << 12 | (rd & 0x1Fu) << 7 | 0x37u;
}

static void put32(emulator_t *emu, u32 addr, u32 val)
{
    memory_write32(&emu->mem, addr, val);
}

static int plic_pass = 0;
static int plic_fail = 0;

#define PLIC_CHECK(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", name); \
        plic_fail++; \
    } else { \
        printf("  [OK]   %s\n", name); \
        plic_pass++; \
    } \
} while (0)

void plic_selftest(void)
{
    emulator_t emu;
    hart_t *h;
    decoded_inst_t inst;
    plic_t *p;
    u32 v;

    plic_pass = 0;
    plic_fail = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        PLIC Self-Test (v0.14)                            ║\n");
    printf("║        priority / enable / threshold / claim / complete  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------- 1. 复位 ---------- */
    printf("── reset state ──\n");
    emu_init(&emu, MEM_SIZE);
    emu_init_harts(&emu, 2);
    h = &emu.harts[0];
    p = &emu.plic;

    PLIC_CHECK(p->pending == 0 && p->enable[0] == 0 && p->threshold[0] == 0,
               "plic_init clears pending / enable / threshold");
    PLIC_CHECK(!plic_pending_for(p, 0), "no interrupt after reset");
    PLIC_CHECK(plic_claim(p, 0) == 0, "claim on empty PLIC returns 0");

    /* ---------- 2. priority = 0 不产生中断 ---------- */
    printf("\n── priority 0 means disabled ──\n");
    plic_raise(p, 3);
    p->enable[0] = 1u << 3;
    PLIC_CHECK((p->pending & (1u << 3)) != 0, "raise sets pending bit");
    PLIC_CHECK(!plic_pending_for(p, 0),
               "priority 0 never interrupts (even if enabled)");

    /* ---------- 3. 优先级 + 使能 ---------- */
    printf("\n── priority and enable ──\n");
    p->priority[3] = 5;
    PLIC_CHECK(plic_pending_for(p, 0), "priority > threshold asserts the line");
    p->enable[0] = 0;
    PLIC_CHECK(!plic_pending_for(p, 0), "clearing enable masks the source");
    p->enable[0] = 1u << 3;
    PLIC_CHECK(plic_pending_for(p, 0), "re-enable restores the source");

    /* ---------- 4. 阈值 ---------- */
    printf("\n── threshold ──\n");
    p->threshold[0] = 5;
    PLIC_CHECK(!plic_pending_for(p, 0),
               "threshold == priority masks (needs strictly greater)");
    p->threshold[0] = 4;
    PLIC_CHECK(plic_pending_for(p, 0), "threshold < priority passes");
    p->threshold[0] = 0;

    /* ---------- 5. claim / complete ---------- */
    printf("\n── claim and complete ──\n");
    v = plic_claim(p, 0);
    PLIC_CHECK(v == 3, "claim returns the pending IRQ");
    PLIC_CHECK((p->pending & (1u << 3)) == 0, "claim clears pending");
    PLIC_CHECK((p->in_service[0] & (1u << 3)) != 0, "claim marks in-service");
    PLIC_CHECK(plic_claim(p, 0) == 0, "second claim returns 0");
    plic_complete(p, 0, 3);
    PLIC_CHECK(p->in_service[0] == 0, "complete clears in-service");
    PLIC_CHECK(!plic_pending_for(p, 0), "edge source stays quiet after complete");

    /* ---------- 6. 优先级排序 + 同优先级取小号 ---------- */
    printf("\n── priority ordering ──\n");
    p->priority[1] = 1;
    p->priority[2] = 7;
    p->priority[4] = 7;
    p->enable[0] = (1u << 1) | (1u << 2) | (1u << 4);
    plic_raise(p, 1);
    plic_raise(p, 2);
    plic_raise(p, 4);
    PLIC_CHECK(plic_claim(p, 0) == 2, "highest priority claimed first");
    PLIC_CHECK(plic_claim(p, 0) == 4, "tie → lower IRQ number first");
    PLIC_CHECK(plic_claim(p, 0) == 1, "lowest priority last");
    PLIC_CHECK(plic_claim(p, 0) == 0, "all drained");

    /* ---------- 7. 每个上下文独立 ---------- */
    printf("\n── per-context enable / threshold ──\n");
    plic_raise(p, 2);
    p->enable[0] = 1u << 2;
    p->enable[1] = 0;
    PLIC_CHECK(plic_pending_for(p, 0), "ctx0 sees its enabled source");
    PLIC_CHECK(!plic_pending_for(p, 1), "ctx1 has it disabled");

    p->enable[1] = 1u << 2;
    p->threshold[1] = 7;
    PLIC_CHECK(!plic_pending_for(p, 1), "ctx1 threshold masks it");
    p->threshold[1] = 0;
    PLIC_CHECK(plic_pending_for(p, 1), "ctx1 threshold lowered");

    /* ---------- 8. MMIO 寄存器 ---------- */
    printf("\n── MMIO registers ──\n");
    PLIC_CHECK(plic_mmio_write(p, PLIC_PRIORITY(2), 9) &&
               p->priority[2] == 9, "priority register write");
    PLIC_CHECK(plic_mmio_read(p, PLIC_PRIORITY(2), &v) && v == 9,
               "priority register read");
    PLIC_CHECK(plic_mmio_read(p, PLIC_PENDING_BASE, &v) &&
               (v & (1u << 2)) != 0, "pending register reflects raise");
    PLIC_CHECK(plic_mmio_write(p, PLIC_PENDING_BASE, 0) &&
               (p->pending & (1u << 2)) != 0,
               "pending is read-only (software cannot clear)");
    PLIC_CHECK(plic_mmio_write(p, PLIC_ENABLE_BASE, 0xFFFFFFFFu) &&
               (p->enable[0] & 1u) == 0,
               "enable bit 0 (IRQ 0) is reserved");
    PLIC_CHECK(plic_mmio_write(p, PLIC_THRESHOLD(0), 3) &&
               p->threshold[0] == 3, "threshold register write");
    PLIC_CHECK(plic_mmio_read(p, PLIC_CLAIM(0), &v) && v == 2,
               "claim register returns IRQ 2");
    PLIC_CHECK(plic_mmio_write(p, PLIC_CLAIM(0), 2) &&
               (p->in_service[0] & (1u << 2)) == 0,
               "claim register write = complete");

    /* ---------- 9. 虚拟设备门铃 ---------- */
    printf("\n── virtual device doorbell ──\n");
    plic_init(p);
    PLIC_CHECK(plic_device_write(p, PLIC_DEV_DOORBELL, (1u << 5) | (1u << 6)),
               "doorbell write accepted");
    PLIC_CHECK((p->pending & ((1u << 5) | (1u << 6))) ==
               ((1u << 5) | (1u << 6)), "doorbell raises both sources");
    PLIC_CHECK(plic_device_write(p, PLIC_DEV_DOORBELL + 4u, 1) == false,
               "unknown device address not claimed");
    plic_clear(p, 5);
    PLIC_CHECK((p->pending & (1u << 5)) == 0, "plic_clear drops a source");

    /* ---------- 10. 接到 mip.MEIP 和 trap ---------- */
    printf("\n── MEIP and trap delivery ──\n");
    emu_free(&emu);
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    p = &emu.plic;

    p->priority[7] = 4;
    p->enable[0] = 1u << 7;
    plic_raise(p, 7);

    h->state = EMU_RUNNING;
    h->csr.mtvec = 0x800;
    h->csr.mie = MIE_MEIE;
    h->csr.mstatus = 0;               /* 先关全局中断 */
    put32(&emu, 0x100, 0x00000013u);  /* nop */
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);
    PLIC_CHECK(h->rf.pc == 0x104, "MIE=0: external IRQ stays pending");
    PLIC_CHECK((h->csr.mip & MIP_MEIP) != 0, "mip.MEIP reflects the PLIC line");

    h->csr.mstatus = MSTATUS_MIE;
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);
    PLIC_CHECK(h->rf.pc == 0x800, "MIE=1: trap to mtvec");
    PLIC_CHECK(h->csr.mcause == MCAUSE_IRQ_EXT,
               "mcause = 0x8000000B (machine external)");
    PLIC_CHECK(h->csr.mepc == 0x100, "mepc = interrupted instruction");
    PLIC_CHECK((h->csr.mstatus & MSTATUS_MIE) == 0, "MIE cleared in handler");

    /*
     * handler 里 claim + complete:
     *   lui  x1, 0x0C200      → x1 = 0x0C200000
     *   lw   x5, 4(x1)        → claim → x5 = 7
     *   sw   x5, 4(x1)        → complete(7)
     *   mret
     */
    put32(&emu, 0x800, enc_lui(0x0C200u, 1));
    put32(&emu, 0x804, enc_i(4, 1, 2, 5, 0x03));   /* lw  x5, 4(x1) */
    put32(&emu, 0x808, enc_s(4, 5, 1, 2));         /* sw  x5, 4(x1) */
    put32(&emu, 0x80C, 0x30200073u);               /* mret          */

    h->rf.pc = 0x800;
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    PLIC_CHECK(h->rf.regs[5] == 7, "handler claims IRQ 7");
    emu_step(&emu, &inst);
    PLIC_CHECK(p->in_service[0] == 0, "handler completes IRQ 7");
    emu_step(&emu, &inst);                          /* mret */
    PLIC_CHECK(h->rf.pc == 0x100, "mret returns to mepc");

    put32(&emu, 0x100, 0x00000013u);
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);
    PLIC_CHECK(h->rf.pc == 0x104, "claimed IRQ does not retrigger");

    /* ---------- 11. 中断优先级: 外部 > 软件 > 定时器 ---------- */
    printf("\n── trap priority ──\n");
    p->priority[9] = 6;
    p->enable[0] = 1u << 9;
    plic_raise(p, 9);
    h->msip = 1;
    h->csr.mip |= MIP_MSIP;
    h->mtimecmp = 1;
    h->csr.mie = MIE_MEIE | MIE_MSIE | MIE_MTIE;
    h->csr.mstatus = MSTATUS_MIE;
    h->rf.pc = 0x200;
    put32(&emu, 0x200, 0x00000013u);
    emu_step(&emu, &inst);
    PLIC_CHECK(h->csr.mcause == MCAUSE_IRQ_EXT,
               "external IRQ has priority over software/timer");

    plic_clear(p, 9);
    p->pending = 0;
    h->msip = 0;
    h->csr.mip &= ~MIP_MSIP;
    h->csr.mstatus = MSTATUS_MIE;
    h->rf.pc = 0x200;
    emu_step(&emu, &inst);
    PLIC_CHECK(h->csr.mcause == MCAUSE_IRQ_TIMER,
               "timer fires after external IRQ is gone");

    printf("\n  Result: %d passed, %d failed\n", plic_pass, plic_fail);
    emu_free(&emu);
}
