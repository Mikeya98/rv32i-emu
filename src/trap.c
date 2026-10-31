/**
 * @file    trap.c
 * @brief   RV32I Emulator — trap 框架实现
 */

#include "trap.h"
#include "csr.h"
#include <stdio.h>

void trap_init(emulator_t *emu)
{
    emu->mtime    = 0;
    emu->mtimecmp = 0;      /* 0 = 定时器关闭 */
    emu->csr.mstatus = 0;
}

bool trap_raise(emulator_t *emu, u32 cause, u32 tval, u32 epc)
{
    csr_t *csr = &emu->csr;
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
    if (emu->priv <= PRIV_S && code < 16u && ((deleg >> code) & 1u)) {
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
        if (emu->priv == PRIV_S) {
            m |= MSTATUS_SPP;
        } else {
            m &= ~MSTATUS_SPP;
        }
        csr->mstatus = m;

        emu->priv = PRIV_S;
        emu->rf.pc = csr->stvec;
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
    m = (m & ~MSTATUS_MPP_MASK) | ((emu->priv & 0x3u) << 11);

    csr->mstatus = m;

    /* 进 trap 一律在 M 态执行 handler */
    emu->priv = PRIV_M;

    emu->rf.pc = csr->mtvec;
    return true;
}

bool trap_check_interrupt(emulator_t *emu, u32 epc)
{
    csr_t *csr = &emu->csr;

    emu->mtime++;

    /*
     * MTIP 是电平整位, 不是锁存的: mtime 一旦追平 mtimecmp 就一直
     * 是 pending, 直到软件把 mtimecmp 写到更远的将来。这和真实
     * CLINT 的行为一致, handler 里不需要手动清 mip。
     */
    if (emu->mtimecmp != 0 && emu->mtime >= emu->mtimecmp) {
        csr->mip |= MIP_MTIP;
    } else {
        csr->mip &= ~MIP_MTIP;
    }

    /*
     * M 态取中断看 mstatus.MIE; U 态可以直接被 M 态中断打断
     * (规范和真机都是这样 —— 低特权级挡不住高特权级的中断)。
     */
    if ((csr->mip & MIP_MTIP) &&
        (csr->mie & MIE_MTIE) &&
        (emu->priv != PRIV_M || (csr->mstatus & MSTATUS_MIE))) {
        return trap_raise(emu, MCAUSE_IRQ_TIMER, 0, epc);
    }

    return false;
}

u32 trap_mret(emulator_t *emu)
{
    csr_t *csr = &emu->csr;
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
    emu->priv = (m & MSTATUS_MPP_MASK) >> 11;
    m = (m & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_U;

    csr->mstatus = m;

    return csr->mepc;
}

u32 trap_sret(emulator_t *emu)
{
    csr_t *csr = &emu->csr;
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

    emu->priv = (m & MSTATUS_SPP) ? PRIV_S : PRIV_U;
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
    emu.csr.mtvec = 0;
    /* 0x00000000: ecall (a7=0 → exit) */
    emu.mem.data[0] = 0x73; emu.mem.data[1] = 0x00;
    emu.mem.data[2] = 0x00; emu.mem.data[3] = 0x00;
    emu.rf.pc = 0;
    emu_state_t st = emu.state;
    emu.state = EMU_RUNNING;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.state == EMU_HALTED, "ecall without handler → halt (exit 0)");
    emu.state = st;

    /* ---------- 2. 装了 handler 后 ecall 进 trap ---------- */
    printf("\n── ecall traps to mtvec ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.csr.mtvec = 0x100;
    emu.csr.mstatus = MSTATUS_MIE;      /* 进 trap 前中断是开的 */
    emu.rf.pc = 0x40;
    emu.mem.data[0x40] = 0x73; emu.mem.data[0x41] = 0x00;
    emu.mem.data[0x42] = 0x00; emu.mem.data[0x43] = 0x00;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x100, "pc jumps to mtvec");
    TRAP_CHECK(emu.csr.mcause == EXC_ECALL_M, "mcause = 11 (ecall from M)");
    TRAP_CHECK(emu.csr.mepc == 0x40, "mepc = faulting pc");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_MIE) == 0, "MIE cleared in handler");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_MPIE) != 0, "MPIE saved MIE");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_M,
               "MPP = M-mode");

    /* ---------- 3. mret 恢复现场 ---------- */
    printf("\n── mret restores state ──\n");
    /* 0x100: mret */
    emu.mem.data[0x100] = 0x73; emu.mem.data[0x101] = 0x00;
    emu.mem.data[0x102] = 0x20; emu.mem.data[0x103] = 0x30;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x40, "mret returns to mepc");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_MIE) != 0, "MIE restored from MPIE");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_MPIE) != 0, "MPIE forced to 1");

    /* ---------- 4. 非法指令 → mcause = 2, mtval = 指令本身 ---------- */
    printf("\n── illegal instruction ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.csr.mtvec = 0x200;
    emu.rf.pc = 0x80;
    /* 0xFFFFFFFF 是无效编码 */
    emu.mem.data[0x80] = 0xFF; emu.mem.data[0x81] = 0xFF;
    emu.mem.data[0x82] = 0xFF; emu.mem.data[0x83] = 0xFF;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x200, "illegal → pc = mtvec");
    TRAP_CHECK(emu.csr.mcause == EXC_ILLEGAL_INST, "mcause = 2");
    TRAP_CHECK(emu.csr.mtval == 0xFFFFFFFFu, "mtval = illegal instruction bits");
    TRAP_CHECK(emu.csr.mepc == 0x80, "mepc = illegal instruction address");

    /* ---------- 5. 定时器中断 ---------- */
    printf("\n── machine timer interrupt ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.csr.mtvec = 0x300;
    emu.csr.mstatus = MSTATUS_MIE;
    emu.csr.mie = MIE_MTIE;
    emu.mtimecmp = 5;
    emu.rf.pc = 0x400;
    /* 0x400: nop (addi x0, x0, 0) */
    emu.mem.data[0x400] = 0x13; emu.mem.data[0x401] = 0x00;
    emu.mem.data[0x402] = 0x00; emu.mem.data[0x403] = 0x00;

    /* 跑 5 步: mtime 从 0 到 5, 第 5 步应该进中断 */
    for (int i = 0; i < 5; i++) {
        emu.rf.pc = 0x400;
        emu_step(&emu, &inst);
        if (emu.rf.pc == 0x300) break;
    }
    TRAP_CHECK(emu.rf.pc == 0x300, "timer IRQ jumps to mtvec");
    TRAP_CHECK(emu.csr.mcause == MCAUSE_IRQ_TIMER, "mcause = 0x80000007");
    TRAP_CHECK((emu.csr.mip & MIP_MTIP) != 0, "mip.MTIP pending");
    TRAP_CHECK(emu.csr.mepc == 0x400, "mepc = next instruction");

    /* handler 里把 mtimecmp 推远 → pending 自动清掉 */
    emu.mtimecmp = emu.mtime + 100;
    emu.rf.pc = 0x400;
    emu_step(&emu, &inst);
    TRAP_CHECK((emu.csr.mip & MIP_MTIP) == 0, "MTIP clears when mtimecmp moves");

    /* ---------- 6. 未安装 handler: 保持旧的未处理异常路径 ---------- */
    printf("\n── no handler installed ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.rf.pc = 0x80;
    emu.mem.data[0x80] = 0xFF; emu.mem.data[0x81] = 0xFF;
    emu.mem.data[0x82] = 0xFF; emu.mem.data[0x83] = 0xFF;
    ok = emu_step(&emu, &inst);
    TRAP_CHECK(!ok && emu.state == EMU_EXCEPTION,
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
    emu.state = EMU_RUNNING;
    emu.priv = PRIV_U;
    emu.csr.medeleg = 1u << EXC_ECALL_U;
    emu.csr.stvec = 0x500;
    emu.csr.mtvec = 0;
    emu.csr.mstatus = MSTATUS_SIE;          /* 进 S handler 前 SIE 是开的 */
    put32(&emu, 0x40, 0x00000073u);         /* ecall */
    emu.rf.pc = 0x40;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x500, "pc = stvec (not mtvec)");
    TRAP_CHECK(emu.csr.scause == EXC_ECALL_U, "scause = 8");
    TRAP_CHECK(emu.csr.sepc == 0x40, "sepc = faulting pc");
    TRAP_CHECK(emu.priv == PRIV_S, "handler runs in S-mode");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_SPP) == 0, "SPP = U (came from U)");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_SIE) == 0, "SIE cleared in handler");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_SPIE) != 0, "SPIE saved SIE");

    /* ---------- 2. sret 回到 U 态 ---------- */
    printf("\n── sret restores state ──\n");
    emu.csr.sepc = 0x44;
    put32(&emu, 0x500, 0x10200073u);        /* sret */
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x44, "sret returns to sepc");
    TRAP_CHECK(emu.priv == PRIV_U, "back to U-mode");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_SIE) != 0, "SIE restored from SPIE");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_SPIE) != 0, "SPIE forced to 1");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_SPP) == 0, "SPP cleared");

    /* ---------- 3. 没有委托的 S 态 ecall 進 M 态 ---------- */
    printf("\n── S ecall without delegation → M ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.priv = PRIV_S;
    emu.csr.mtvec = 0x600;
    emu.csr.stvec = 0x500;
    emu.csr.medeleg = 0;
    put32(&emu, 0x100, 0x00000073u);
    emu.rf.pc = 0x100;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x600, "pc = mtvec");
    TRAP_CHECK(emu.csr.mcause == EXC_ECALL_S, "mcause = 9");
    TRAP_CHECK((emu.csr.mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_S,
               "MPP = S");
    TRAP_CHECK(emu.priv == PRIV_M, "handler runs in M-mode");

    put32(&emu, 0x600, 0x30200073u);        /* mret */
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x100, "mret returns to S-mode pc");
    TRAP_CHECK(emu.priv == PRIV_S, "back to S-mode");

    /* ---------- 4. U 态非法指令委托给 S ---------- */
    printf("\n── delegated illegal instruction ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.priv = PRIV_U;
    emu.csr.medeleg = 1u << EXC_ILLEGAL_INST;
    emu.csr.stvec = 0x500;
    put32(&emu, 0x80, 0xFFFFFFFFu);
    emu.rf.pc = 0x80;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x500, "illegal → S handler");
    TRAP_CHECK(emu.csr.scause == EXC_ILLEGAL_INST, "scause = 2");
    TRAP_CHECK(emu.csr.stval == 0xFFFFFFFFu, "stval = instruction bits");

    /* ---------- 5. CSR 权限: U 不能读 sstatus, S 可以 ---------- */
    printf("\n── CSR privilege ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;
    emu.priv = PRIV_U;
    emu.csr.medeleg = 1u << EXC_ILLEGAL_INST;
    emu.csr.stvec = 0x500;
    put32(&emu, 0x200, enc_csrrs(5, CSR_SSTATUS));
    emu.rf.pc = 0x200;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x500, "U reading sstatus → illegal → S");
    TRAP_CHECK(emu.csr.scause == EXC_ILLEGAL_INST, "scause = 2");

    emu.priv = PRIV_S;
    emu.rf.pc = 0x200;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x204, "S reading sstatus is allowed");
    TRAP_CHECK(emu.rf.regs[5] == (emu.csr.mstatus & SSTATUS_MASK),
               "sstatus read returns the S view");

    /* ---------- 6. sfence.vma: S 可以, U 不可以 ---------- */
    printf("\n── sfence.vma privilege ──\n");
    put32(&emu, 0x300, 0x12000073u);
    emu.priv = PRIV_S;
    emu.rf.pc = 0x300;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x304, "S executes sfence.vma");

    emu.priv = PRIV_U;
    emu.rf.pc = 0x300;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x500, "U executing sfence.vma → illegal → S");
    TRAP_CHECK(emu.csr.scause == EXC_ILLEGAL_INST, "scause = 2");

    /* ---------- 7. sret 不能在 U 态执行 ---------- */
    printf("\n── sret privilege ──\n");
    put32(&emu, 0x400, 0x10200073u);
    emu.priv = PRIV_U;
    emu.rf.pc = 0x400;
    emu_step(&emu, &inst);
    TRAP_CHECK(emu.rf.pc == 0x500, "U executing sret → illegal → S");
    TRAP_CHECK(emu.csr.scause == EXC_ILLEGAL_INST, "scause = 2");

    printf("\n  Result: %d passed, %d failed\n", trap_pass, trap_fail);
}
