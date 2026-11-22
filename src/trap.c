/**
 * @file    trap.c
 * @brief   RV32I Emulator — trap 框架实现
 */

#include "trap.h"
#include "csr.h"
#include <stdio.h>

bool trap_raise(emulator_t *emu, u32 cause, u32 tval, u32 epc)
{
    hart_t *h = emu_hart(emu);
    csr_t *csr = &h->csr;
    u32 m;
    u32 code = cause & ~MCAUSE_IRQ_BIT;
    bool irq = (cause & MCAUSE_IRQ_BIT) != 0;
    u32 deleg = irq ? csr->mideleg : csr->medeleg;

    /*
     * 委托 (v0.5): 当前级不高于 S, 且这一位在 medeleg/mideleg 里
     * 打开时, trap 直接进 S 态 handler, 不经过 M 态。
     *
     * S 态的现场用 sstatus.SPP / sepc / scause / stval 保存, 和 M 态
     * 那套完全分开 —— 这样 M 态 handler 正在处理一个 trap 时, 被
     * 委托的另一个 trap 不会覆盖它的 mepc。
     */
    if (h->priv <= PRIV_S && code < 16u && ((deleg >> code) & 1u)) {
        if (csr->stvec == 0) {
            return false;
        }

        csr->sepc   = epc & ~0x3u;
        csr->scause = cause;
        csr->stval  = tval;

        m = csr->mstatus;
        if (m & MSTATUS_SIE) {
            m |= MSTATUS_SPIE;
        } else {
            m &= ~MSTATUS_SPIE;
        }
        m &= ~MSTATUS_SIE;
        if (h->priv == PRIV_S) {
            m |= MSTATUS_SPP;
        } else {
            m &= ~MSTATUS_SPP;
        }
        csr->mstatus = m;

        h->priv = PRIV_S;
        h->rf.pc = csr->stvec;
        return true;
    }

    if (csr->mtvec == 0) {
        return false;       /* 没装 handler: 由调用者按未处理异常处理 */
    }

    csr->mepc   = epc & ~0x3u;
    csr->mcause = cause;
    csr->mtval  = tval;

    /*
     * mstatus 的三个位:
     *   MPIE ← MIE   记住进 trap 之前中断是开还是关
     *   MIE  ← 0     进 handler 之后先关中断, 防止嵌套
     *   MPP  ← 当前特权级
     *
     * v0.4 之后 MPP 不能写死成 M: U 态来的 trap 记 00, mret 才能
     * 正确回到 U 态。写死 11 的话 mret 永远回 M, 用户态根本出不去。
     */
    m = csr->mstatus;
    if (m & MSTATUS_MIE) {
        m |= MSTATUS_MPIE;
    } else {
        m &= ~MSTATUS_MPIE;
    }
    m &= ~MSTATUS_MIE;
    m = (m & ~MSTATUS_MPP_MASK) | ((h->priv & 0x3u) << 11);

    csr->mstatus = m;

    /* 进 trap 一律在 M 态执行 handler */
    h->priv = PRIV_M;

    h->rf.pc = csr->mtvec;
    return true;
}

bool trap_check_interrupt(emulator_t *emu, u32 epc)
{
    hart_t *h = emu_hart(emu);
    csr_t *csr = &h->csr;

    emu->mtime++;

    /*
     * MTIP 是电平整位, 不是锁存的: mtime 一旦追平 mtimecmp 就一直
     * 是 pending, 直到软件把 mtimecmp 写到更远的将来。这和真实
     * CLINT 的行为一致, handler 里不需要手动清 mip。
     */
    if (h->mtimecmp != 0 && emu->mtime >= h->mtimecmp) {
        csr->mip |= MIP_MTIP;
    } else {
        csr->mip &= ~MIP_MTIP;
    }

    /*
     * v0.16 (Sstc): STIP 也是一个电平信号, 有两个来源 ——
     *   - S 态通过 stimecmp 装了一个比较值, mtime 追平就拉起来
     *   - M 态软件直接写 mip.STIP (模拟 M 态固件转发定时器中断)
     * 两个来源"或"在一起, 和真机上多个中断源共用一个 pending 位一致。
     */
    {
        bool stip = (csr->stip_sw != 0) ||
                    (csr->stimecmp != 0 && emu->mtime >= csr->stimecmp);

        if (stip) {
            csr->mip |= MIP_STIP;
        } else {
            csr->mip &= ~MIP_STIP;
        }
    }

    /*
     * v0.13: 软件中断也是电平信号, 由 CLINT msip 寄存器拉起来。
     * 这里每步同步一次 mip, 保证软件读到的 pending 位和 CLINT 一致。
     */
    if (h->msip) {
        csr->mip |= MIP_MSIP;
    } else {
        csr->mip &= ~MIP_MSIP;
    }

    /*
     * v0.14: 外部中断线由 PLIC 汇总。MEIP 是"线"而不是锁存的 pending:
     * 只要还存在一条 使能 & 优先级 > 阈值 的 pending 源, MEIP 就为 1。
     * claim 把 pending 清掉之后, 下一步这里就会自动把它拉低 ——
     * 不需要软件手动清 mip。
     *
     * v0.15: 每个 hart 有 M/S 两个 PLIC 上下文。M 上下文拉 MEIP,
     * S 上下文拉 SEIP (sip.SEIP 是它的 S 态视图)。两条线各自独立:
     * 设备挂在哪个上下文上, 就由哪一级处理。
     */
    {
        bool ext_m = plic_pending_for(&emu->plic, PLIC_CTX_M(emu->cur));
        bool ext_s = plic_pending_for(&emu->plic, PLIC_CTX_S(emu->cur));

        csr->mip = (csr->mip & ~(MIP_MEIP | MIP_SEIP)) |
                   (ext_m ? MIP_MEIP : 0u) | (ext_s ? MIP_SEIP : 0u);

        /* sip 是 mip 在 S 态可见的那几位: SEIP 来自 PLIC, STIP 来自上一段 */
        csr->sip = (csr->sip & ~(MIP_SEIP | MIP_STIP)) |
                   (ext_s ? MIP_SEIP : 0u) |
                   ((csr->mip & MIP_STIP) ? MIP_STIP : 0u);
    }

    /*
     * M 态取中断看 mstatus.MIE; U 态可以直接被 M 态中断打断
     * (规范和真机都是这样 —— 低特权级挡不住高特权级的中断)。
     * S 态中断在 U 态照常可以打断, 在 S 态看 sstatus.SIE。
     */
    {
        bool m_ok = (h->priv != PRIV_M) || (csr->mstatus & MSTATUS_MIE);
        bool s_ok = (h->priv == PRIV_U) ||
                    (h->priv == PRIV_S && (csr->mstatus & MSTATUS_SIE));

        /*
         * 优先级按规范: 外部 > 软件 > 定时器。
         * S 态外部中断排在机器级中断之后: 它属于低一级的目标。
         */
        if (m_ok) {
            if ((csr->mip & MIP_MEIP) && (csr->mie & MIE_MEIE)) {
                return trap_raise(emu, MCAUSE_IRQ_EXT, 0, epc);
            }
            if ((csr->mip & MIP_MSIP) && (csr->mie & MIE_MSIE)) {
                return trap_raise(emu, MCAUSE_IRQ_SOFT, 0, epc);
            }
            if ((csr->mip & MIP_MTIP) && (csr->mie & MIE_MTIE)) {
                return trap_raise(emu, MCAUSE_IRQ_TIMER, 0, epc);
            }
        }

        /*
         * S 级中断: 外部优先于定时器。
         *
         * mideleg[9/5] = 1: 委托给 S 态, 由 sie 里对应使能位放行,
         *                   trap_raise 看到委托位就会直接进 stvec。
         * mideleg[9/5] = 0: 没委托, 由 M 态用 mie 里同一个位接管,
         *                   mcause/scause 的编号不变, 处理完 mret 回去。
         */
        if (csr->mip & MIP_SEIP) {
            if (csr->mideleg & MIP_SEIP) {
                if (s_ok && (csr->sie & MIE_SEIE)) {
                    return trap_raise(emu, MCAUSE_IRQ_S_EXT, 0, epc);
                }
            } else if (m_ok && (csr->mie & MIE_SEIE)) {
                return trap_raise(emu, MCAUSE_IRQ_S_EXT, 0, epc);
            }
        }

        if (csr->mip & MIP_STIP) {
            /*
             * v0.16: 委托出去的 S 定时器中断在 M 态不投递 —— 和 S 外部
             * 中断一样, delegated 中断只打断 S/U 态。M 态自己用 STIE
             * 接管的那条路径则看 mstatus.MIE。
             */
            if (csr->mideleg & MIP_STIP) {
                if (s_ok && (csr->sie & MIE_STIE)) {
                    return trap_raise(emu, MCAUSE_IRQ_S_TIMER, 0, epc);
                }
            } else if (m_ok && (csr->mie & MIE_STIE)) {
                return trap_raise(emu, MCAUSE_IRQ_S_TIMER, 0, epc);
            }
        }
    }

    return false;
}

/*
 * WFI 的唤醒条件 (v0.15)。
 *
 * 规范要求 WFI 在"本 hart 有中断 pending 且已使能"时恢复执行,
 * 不管当时的全局中断开关是什么 —— 如果只是 mstatus.MIE/SIE 关着,
 * 醒来之后继续往下跑, 不一定要立刻进 trap。
 * 这里把 M 级 (mip & mie) 和 S 级 (sip & sie) 都算上。
 */
bool trap_irq_pending(emulator_t *emu)
{
    hart_t *h = emu_hart(emu);
    csr_t *csr = &h->csr;

    if (csr->mip & csr->mie &
        (MIP_MEIP | MIP_MSIP | MIP_MTIP | MIP_SEIP | MIP_STIP)) {
        return true;
    }
    if (csr->sip & csr->sie & (MIP_SEIP | MIP_STIP)) {
        return true;
    }
    return false;
}

u32 trap_mret(emulator_t *emu)
{
    hart_t *h = emu_hart(emu);
    csr_t *csr = &h->csr;
    u32 m = csr->mstatus;

    if (m & MSTATUS_MPIE) {
        m |= MSTATUS_MIE;
    } else {
        m &= ~MSTATUS_MIE;
    }
    m |= MSTATUS_MPIE;                      /* mret 之后 MPIE 恒为 1 */

    /*
     * 特权级回到 MPP 记录的那一级, MPP 复位成"所支持的最低特权级"。
     * 本模拟器只支持 M/U, 所以 MPP 清成 00 (U)。
     */
    h->priv = (m & MSTATUS_MPP_MASK) >> 11;
    m = (m & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_U;

    csr->mstatus = m;

    return csr->mepc;
}

u32 trap_sret(emulator_t *emu)
{
    hart_t *h = emu_hart(emu);
    csr_t *csr = &h->csr;
    u32 m = csr->mstatus;

    /*
     * sret 的规则和 mret 对称:
     *   SIE ← SPIE, SPIE ← 1, SPP ← 0
     *   特权级回到 SPP 记录的那一级 (1=S, 0=U)
     */
    if (m & MSTATUS_SPIE) {
        m |= MSTATUS_SIE;
    } else {
        m &= ~MSTATUS_SIE;
    }
    m |= MSTATUS_SPIE;

    h->priv = (m & MSTATUS_SPP) ? PRIV_S : PRIV_U;
    m &= ~MSTATUS_SPP;

    csr->mstatus = m;

    return csr->sepc;
}

/* =========================== 自测 =========================== */

static int trap_pass = 0;
static int trap_fail = 0;

#define TRAP_CHECK(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", name); \
        trap_fail++; \
    } else { \
        printf("  [OK]   %s\n", name); \
        trap_pass++; \
    } \
} while (0)

void trap_selftest(void)
{
    emulator_t emu;
    hart_t *h = &emu.harts[0];
    decoded_inst_t inst;
    u32 val;
    u32 pc;
    bool ok;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        Trap Framework Self-Test                          ║\n");
    printf("║        ecall / illegal / timer IRQ / mret                ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------- 1. 没装 handler 时 ecall 保持旧的 syscall 行为 ---------- */
    printf("── ecall without mtvec (legacy syscall path) ──\n");
    emu_init(&emu, MEM_SIZE);
    h->csr.mtvec = 0;
    /* 0x00000000: ecall (a7=0 → exit) */
    emu.mem.data[0] = 0x73; emu.mem.data[1] = 0x00;
    emu.mem.data[2] = 0x00; emu.mem.data[3] = 0x00;
    h->rf.pc = 0;
    emu_state_t st = h->state;
    h->state = EMU_RUNNING;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->state == EMU_HALTED, "ecall without handler → halt (exit 0)");
    h->state = st;

    /* ---------- 2. 装了 handler 后 ecall 进 trap ---------- */
    printf("\n── ecall traps to mtvec ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->csr.mtvec = 0x100;
    h->csr.mstatus = MSTATUS_MIE;      /* 进 trap 前中断是开的 */
    h->rf.pc = 0x40;
    emu.mem.data[0x40] = 0x73; emu.mem.data[0x41] = 0x00;
    emu.mem.data[0x42] = 0x00; emu.mem.data[0x43] = 0x00;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x100, "pc jumps to mtvec");
    TRAP_CHECK(h->csr.mcause == EXC_ECALL_M, "mcause = 11 (ecall from M)");
    TRAP_CHECK(h->csr.mepc == 0x40, "mepc = faulting pc");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MIE) == 0, "MIE cleared in handler");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MPIE) != 0, "MPIE saved MIE");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_M,
               "MPP = M-mode");

    /* ---------- 3. mret 恢复现场 ---------- */
    printf("\n── mret restores state ──\n");
    /* 0x100: mret */
    emu.mem.data[0x100] = 0x73; emu.mem.data[0x101] = 0x00;
    emu.mem.data[0x102] = 0x20; emu.mem.data[0x103] = 0x30;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x40, "mret returns to mepc");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MIE) != 0, "MIE restored from MPIE");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MPIE) != 0, "MPIE forced to 1");

    /* ---------- 4. 非法指令 → mcause = 2, mtval = 指令本身 ---------- */
    printf("\n── illegal instruction ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->csr.mtvec = 0x200;
    h->rf.pc = 0x80;
    /* 0xFFFFFFFF 是无效编码 */
    emu.mem.data[0x80] = 0xFF; emu.mem.data[0x81] = 0xFF;
    emu.mem.data[0x82] = 0xFF; emu.mem.data[0x83] = 0xFF;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x200, "illegal → pc = mtvec");
    TRAP_CHECK(h->csr.mcause == EXC_ILLEGAL_INST, "mcause = 2");
    TRAP_CHECK(h->csr.mtval == 0xFFFFFFFFu, "mtval = illegal instruction bits");
    TRAP_CHECK(h->csr.mepc == 0x80, "mepc = illegal instruction address");

    /* ---------- 5. 定时器中断 ---------- */
    printf("\n── machine timer interrupt ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->csr.mtvec = 0x300;
    h->csr.mstatus = MSTATUS_MIE;
    h->csr.mie = MIE_MTIE;
    h->mtimecmp = 5;
    h->rf.pc = 0x400;
    /* 0x400: nop (addi x0, x0, 0) */
    emu.mem.data[0x400] = 0x13; emu.mem.data[0x401] = 0x00;
    emu.mem.data[0x402] = 0x00; emu.mem.data[0x403] = 0x00;

    /* 跑 5 步: mtime 从 0 到 5, 第 5 步应该进中断 */
    for (int i = 0; i < 5; i++) {
        h->rf.pc = 0x400;
        emu_step(&emu, &inst);
        if (h->rf.pc == 0x300) break;
    }
    TRAP_CHECK(h->rf.pc == 0x300, "timer IRQ jumps to mtvec");
    TRAP_CHECK(h->csr.mcause == MCAUSE_IRQ_TIMER, "mcause = 0x80000007");
    TRAP_CHECK((h->csr.mip & MIP_MTIP) != 0, "mip.MTIP pending");
    TRAP_CHECK(h->csr.mepc == 0x400, "mepc = next instruction");

    /* handler 里把 mtimecmp 推远 → pending 自动清掉 */
    h->mtimecmp = emu.mtime + 100;
    h->rf.pc = 0x400;
    emu_step(&emu, &inst);
    TRAP_CHECK((h->csr.mip & MIP_MTIP) == 0, "MTIP clears when mtimecmp moves");

    /* ---------- 6. 未安装 handler: 保持旧的未处理异常路径 ---------- */
    printf("\n── no handler installed ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->rf.pc = 0x80;
    emu.mem.data[0x80] = 0xFF; emu.mem.data[0x81] = 0xFF;
    emu.mem.data[0x82] = 0xFF; emu.mem.data[0x83] = 0xFF;
    ok = emu_step(&emu, &inst);
    TRAP_CHECK(!ok && h->state == EMU_EXCEPTION,
               "no mtvec → illegal instruction is fatal");

    (void)val;
    (void)pc;

    printf("\n  Result: %d passed, %d failed\n", trap_pass, trap_fail);
}

/* =========================== S 态 / 委托自测 =========================== */

static void put32(emulator_t *emu, u32 addr, u32 val)
{
    emu->mem.data[addr + 0] = (u8)(val & 0xFF);
    emu->mem.data[addr + 1] = (u8)((val >> 8) & 0xFF);
    emu->mem.data[addr + 2] = (u8)((val >> 16) & 0xFF);
    emu->mem.data[addr + 3] = (u8)((val >> 24) & 0xFF);
}

/* csrrs rd, csr, x0 —— 读一个 CSR */
static u32 enc_csrrs(u32 rd, u32 csr)
{
    return (csr << 20) | (2u << 12) | (rd << 7) | 0x73u;
}

void trap_smode_selftest(void)
{
    emulator_t emu;
    hart_t *h = &emu.harts[0];
    decoded_inst_t inst;

    trap_pass = 0;
    trap_fail = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        S-mode / Delegation Self-Test (v0.5)              ║\n");
    printf("║        medeleg / stvec / scause / sret / CSR 权限        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------- 1. U 态 ecall 委托给 S 态 ---------- */
    printf("── delegated ecall: U → S ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->priv = PRIV_U;
    h->csr.medeleg = 1u << EXC_ECALL_U;
    h->csr.stvec = 0x500;
    h->csr.mtvec = 0;
    h->csr.mstatus = MSTATUS_SIE;          /* 进 S handler 前 SIE 是开的 */
    put32(&emu, 0x40, 0x00000073u);         /* ecall */
    h->rf.pc = 0x40;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x500, "pc = stvec (not mtvec)");
    TRAP_CHECK(h->csr.scause == EXC_ECALL_U, "scause = 8");
    TRAP_CHECK(h->csr.sepc == 0x40, "sepc = faulting pc");
    TRAP_CHECK(h->priv == PRIV_S, "handler runs in S-mode");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SPP) == 0, "SPP = U (came from U)");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SIE) == 0, "SIE cleared in handler");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SPIE) != 0, "SPIE saved SIE");

    /* ---------- 2. sret 回到 U 态 ---------- */
    printf("\n── sret restores state ──\n");
    h->csr.sepc = 0x44;
    put32(&emu, 0x500, 0x10200073u);        /* sret */
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x44, "sret returns to sepc");
    TRAP_CHECK(h->priv == PRIV_U, "back to U-mode");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SIE) != 0, "SIE restored from SPIE");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SPIE) != 0, "SPIE forced to 1");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SPP) == 0, "SPP cleared");

    /* ---------- 3. 没有委托的 S 态 ecall 進 M 态 ---------- */
    printf("\n── S ecall without delegation → M ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->priv = PRIV_S;
    h->csr.mtvec = 0x600;
    h->csr.stvec = 0x500;
    h->csr.medeleg = 0;
    put32(&emu, 0x100, 0x00000073u);
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x600, "pc = mtvec");
    TRAP_CHECK(h->csr.mcause == EXC_ECALL_S, "mcause = 9");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_S,
               "MPP = S");
    TRAP_CHECK(h->priv == PRIV_M, "handler runs in M-mode");

    put32(&emu, 0x600, 0x30200073u);        /* mret */
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x100, "mret returns to S-mode pc");
    TRAP_CHECK(h->priv == PRIV_S, "back to S-mode");

    /* ---------- 4. U 态非法指令委托给 S ---------- */
    printf("\n── delegated illegal instruction ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->priv = PRIV_U;
    h->csr.medeleg = 1u << EXC_ILLEGAL_INST;
    h->csr.stvec = 0x500;
    put32(&emu, 0x80, 0xFFFFFFFFu);
    h->rf.pc = 0x80;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x500, "illegal → S handler");
    TRAP_CHECK(h->csr.scause == EXC_ILLEGAL_INST, "scause = 2");
    TRAP_CHECK(h->csr.stval == 0xFFFFFFFFu, "stval = instruction bits");

    /* ---------- 5. CSR 权限: U 不能读 sstatus, S 可以 ---------- */
    printf("\n── CSR privilege ──\n");
    emu_init(&emu, MEM_SIZE);
    h->state = EMU_RUNNING;
    h->priv = PRIV_U;
    h->csr.medeleg = 1u << EXC_ILLEGAL_INST;
    h->csr.stvec = 0x500;
    put32(&emu, 0x200, enc_csrrs(5, CSR_SSTATUS));
    h->rf.pc = 0x200;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x500, "U reading sstatus → illegal → S");
    TRAP_CHECK(h->csr.scause == EXC_ILLEGAL_INST, "scause = 2");

    h->priv = PRIV_S;
    h->rf.pc = 0x200;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x204, "S reading sstatus is allowed");
    TRAP_CHECK(h->rf.regs[5] == (h->csr.mstatus & SSTATUS_MASK),
               "sstatus read returns the S view");

    /* ---------- 6. sfence.vma: S 可以, U 不可以 ---------- */
    printf("\n── sfence.vma privilege ──\n");
    put32(&emu, 0x300, 0x12000073u);
    h->priv = PRIV_S;
    h->rf.pc = 0x300;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x304, "S executes sfence.vma");

    h->priv = PRIV_U;
    h->rf.pc = 0x300;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x500, "U executing sfence.vma → illegal → S");
    TRAP_CHECK(h->csr.scause == EXC_ILLEGAL_INST, "scause = 2");

    /* ---------- 7. sret 不能在 U 态执行 ---------- */
    printf("\n── sret privilege ──\n");
    put32(&emu, 0x400, 0x10200073u);
    h->priv = PRIV_U;
    h->rf.pc = 0x400;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x500, "U executing sret → illegal → S");
    TRAP_CHECK(h->csr.scause == EXC_ILLEGAL_INST, "scause = 2");

    printf("\n  Result: %d passed, %d failed\n", trap_pass, trap_fail);
}

/* ====================== S 态定时器中断自测 (v0.16) ====================== */

void trap_stimer_selftest(void)
{
    emulator_t emu;
    hart_t *h;
    decoded_inst_t inst;
    bool ok;
    u32 v;

    trap_pass = 0;
    trap_fail = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        Supervisor Timer Self-Test (v0.16)                ║\n");
    printf("║        STIP / stimecmp (Sstc) / delegation / WFI         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------- 1. stimecmp 到点拉高 STIP ---------- */
    printf("── stimecmp raises STIP when mtime catches up ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->csr.stimecmp = emu.mtime + 2;
    put32(&emu, 0x100, 0x00000013u);        /* nop */
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);                  /* mtime 0 → 1 */
    TRAP_CHECK((h->csr.mip & MIP_STIP) == 0,
               "STIP stays low before mtime reaches stimecmp");
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);                  /* mtime 1 → 2 */
    TRAP_CHECK((h->csr.mip & MIP_STIP) != 0,
               "STIP rises when mtime reaches stimecmp");
    TRAP_CHECK((h->csr.sip & MIP_STIP) != 0, "sip.STIP mirrors mip.STIP");

    h->csr.stimecmp = 0;                    /* 关掉定时器 */
    h->rf.pc = 0x100;
    emu_step(&emu, &inst);
    TRAP_CHECK((h->csr.mip & MIP_STIP) == 0, "clearing stimecmp drops STIP");

    /* ---------- 2. 委托给 S 态 ---------- */
    printf("\n── delegated STIP traps to stvec ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->priv       = PRIV_S;
    h->csr.stvec  = 0x500;
    h->csr.mtvec  = 0x600;
    h->csr.mideleg = MIP_STIP;
    h->csr.sie    = MIE_STIE;
    h->csr.mstatus = MSTATUS_SIE;
    h->csr.stimecmp = emu.mtime + 1;
    put32(&emu, 0x40, 0x00000013u);         /* nop */
    h->rf.pc = 0x40;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x500, "pc = stvec");
    TRAP_CHECK(h->csr.scause == MCAUSE_IRQ_S_TIMER, "scause = 0x80000005");
    TRAP_CHECK(h->csr.sepc == 0x40, "sepc = interrupted instruction");
    TRAP_CHECK(h->priv == PRIV_S, "handler runs in S-mode");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SIE) == 0, "SIE cleared in handler");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_SPIE) != 0, "SPIE saved SIE");
    TRAP_CHECK((h->csr.sip & MIP_STIP) != 0,
               "sip.STIP still pending inside the handler");

    /* sret 回原指令; 先撤掉定时器, 否则立刻又进 */
    h->csr.stimecmp = 0;
    h->csr.sie = 0;
    h->csr.sepc = 0x44;
    put32(&emu, 0x500, 0x10200073u);        /* sret */
    h->rf.pc = 0x500;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x44 && h->priv == PRIV_S, "sret returns to sepc");

    /* ---------- 3. 没有委托: M 态接管 ---------- */
    printf("\n── non-delegated STIP is taken in M-mode ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->priv = PRIV_M;
    h->csr.mtvec = 0x600;
    h->csr.mideleg = 0;
    h->csr.mie = MIE_STIE;
    h->csr.mstatus = MSTATUS_MIE;
    h->csr.stimecmp = emu.mtime + 1;
    put32(&emu, 0x200, 0x00000013u);
    h->rf.pc = 0x200;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x600, "STIP without mideleg goes to mtvec");
    TRAP_CHECK(h->csr.mcause == MCAUSE_IRQ_S_TIMER,
               "mcause = 0x80000005 in M-mode");
    TRAP_CHECK((h->csr.mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_M,
               "MPP = M");

    /* ---------- 4. 委托出去的 STIP 不打断 M 态 ---------- */
    printf("\n── delegated STIP does not preempt M-mode ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->priv = PRIV_M;
    h->csr.mtvec = 0x600;
    h->csr.stvec = 0x500;
    h->csr.mideleg = MIP_STIP;
    h->csr.sie = MIE_STIE;
    h->csr.mie = MIE_STIE;
    h->csr.mstatus = MSTATUS_MIE;
    h->csr.stimecmp = emu.mtime + 1;
    put32(&emu, 0x200, 0x00000013u);
    h->rf.pc = 0x200;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->rf.pc == 0x204, "M-mode keeps executing, pc advances");
    TRAP_CHECK((h->csr.mip & MIP_STIP) != 0, "STIP stays pending for S-mode");

    /* ---------- 5. M 态软件直接写 mip.STIP ---------- */
    printf("\n── M-mode software raises mip.STIP ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    csr_write(&h->csr, CSR_MIP, MIP_STIP, &ok);
    TRAP_CHECK(ok && h->csr.stip_sw == 1 && (h->csr.mip & MIP_STIP) != 0,
               "writing mip.STIP sets the software source");
    csr_write(&h->csr, CSR_MIP, 0, &ok);
    TRAP_CHECK((h->csr.mip & MIP_STIP) == 0, "writing 0 clears it");

    /* S 态只能用 SIP 写 SSIP, STIP 对它是只读的 */
    h->csr.sip = MIP_STIP;
    csr_write(&h->csr, CSR_SIP, 0, &ok);
    TRAP_CHECK((h->csr.sip & MIP_STIP) != 0,
               "S-mode cannot clear sip.STIP");

    /* ---------- 6. stimecmp / stimecmph 拼成 64 位 ---------- */
    printf("\n── stimecmp is a 64-bit CSR on RV32 ──\n");
    h->csr.stimecmp = 0;
    csr_write(&h->csr, CSR_STIMECMP, 0x11223344u, &ok);
    csr_write(&h->csr, CSR_STIMECMPH, 0x55667788u, &ok);
    TRAP_CHECK(h->csr.stimecmp == 0x5566778811223344ULL,
               "stimecmp/h assemble into one 64-bit value");
    TRAP_CHECK(csr_read(&h->csr, CSR_STIMECMP, &ok) == 0x11223344u,
               "reading stimecmp returns the low word");
    TRAP_CHECK(csr_read(&h->csr, CSR_STIMECMPH, &ok) == 0x55667788u,
               "reading stimecmph returns the high word");

    /* ---------- 7. WFI 被 STIP 唤醒 ---------- */
    printf("\n── WFI wakes on STIP ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->priv = PRIV_S;
    h->csr.stvec = 0x500;
    h->csr.mideleg = MIP_STIP;
    h->csr.sie = MIE_STIE;
    h->csr.mstatus = 0;                    /* 全局中断关着: 只唤醒, 不立刻进 trap */
    h->csr.stimecmp = emu.mtime + 3;
    put32(&emu, 0x300, 0x10500073u);       /* wfi */
    put32(&emu, 0x304, 0x00000013u);       /* nop */
    put32(&emu, 0x500, 0x10200073u);       /* sret (用不上) */
    h->rf.pc = 0x300;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->waiting, "wfi enters the wait state");
    emu_run(&emu, 8);
    TRAP_CHECK(!h->waiting, "STIP wakes the hart");
    TRAP_CHECK((h->csr.mip & MIP_STIP) != 0, "mip.STIP is pending after the wake");

    /* SIE=1 时醒来直接进 S handler */
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->priv = PRIV_S;
    h->csr.stvec = 0x500;
    h->csr.mideleg = MIP_STIP;
    h->csr.sie = MIE_STIE;
    h->csr.mstatus = MSTATUS_SIE;
    h->csr.stimecmp = emu.mtime + 3;
    put32(&emu, 0x300, 0x10500073u);
    put32(&emu, 0x304, 0x00000013u);
    put32(&emu, 0x500, 0x10200073u);
    h->rf.pc = 0x300;
    emu_step(&emu, &inst);
    emu_run(&emu, 8);
    TRAP_CHECK(h->rf.pc == 0x500, "waking with SIE=1 traps to stvec");
    TRAP_CHECK(h->csr.scause == MCAUSE_IRQ_S_TIMER, "scause = 0x80000005");
    TRAP_CHECK(h->csr.sepc == 0x304, "sepc = instruction after wfi");

    /* ---------- 8. 中断优先级: SEI 先于 STI ---------- */
    printf("\n── supervisor external beats supervisor timer ──\n");
    emu_init(&emu, MEM_SIZE);
    h = &emu.harts[0];
    h->state = EMU_RUNNING;
    h->priv = PRIV_S;
    h->csr.stvec = 0x500;
    h->csr.mideleg = MIP_SEIP | MIP_STIP;
    h->csr.sie = MIE_SEIE | MIE_STIE;
    h->csr.mstatus = MSTATUS_SIE;
    h->csr.stimecmp = emu.mtime + 1;
    emu.plic.priority[2] = 3;
    emu.plic.enable[PLIC_CTX_S(0)] = 1u << 2;
    plic_raise(&emu.plic, 2);
    put32(&emu, 0x40, 0x00000013u);
    h->rf.pc = 0x40;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->csr.scause == MCAUSE_IRQ_S_EXT, "SEI is delivered first");

    /*
     * handler 里 claim + complete 掉 IRQ2 (读 claim 才清 pending),
     * 再把 SIE 打开 —— 相当于 sret 恢复了中断。下一个边界 STIP 还在,
     * 这时应该轮到定时器中断。
     */
    v = 0;
    plic_mmio_read(&emu.plic, PLIC_CLAIM(PLIC_CTX_S(0)), &v);
    plic_mmio_write(&emu.plic, PLIC_CLAIM(PLIC_CTX_S(0)), v);
    h->csr.mstatus |= MSTATUS_SIE;
    put32(&emu, 0x500, 0x00000013u);
    h->rf.pc = 0x500;
    emu_step(&emu, &inst);
    TRAP_CHECK(h->csr.scause == MCAUSE_IRQ_S_TIMER,
               "STI follows once SEI is completed");

    (void)v;
    printf("\n  Result: %d passed, %d failed\n", trap_pass, trap_fail);
}
