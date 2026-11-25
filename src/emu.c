/**
 * @file    emu.c
 * @brief   RV32I Emulator — 顶层模拟器实现
 *
 * fetch-decode-execute 主循环 + 多 hart 轮转调度。
 */

#include "emu.h"
#include "elf.h"
#include "execute.h"
#include "trap.h"
#include "mmu.h"
#include <stdio.h>
#include <string.h>

/* =========================== 初始化 =========================== */

/**
 * @brief 复位一个 hart
 *
 * 真实 CPU 复位后所有 hart 都从同一个复位向量开始跑, 固件读
 * mhartid 决定自己是谁。这里保持一样的行为: 每个 hart 都复位
 * 进 M 态, mhartid 写入编号, pc 等加载 ELF 时再统一设置。
 */
static void hart_reset(hart_t *h, u32 hartid)
{
    regfile_init(&h->rf);
    csr_init(&h->csr);

    h->csr.mhartid = hartid;
    h->csr.mstatus = MSTATUS_FS_INITIAL;    /* v0.11: 复位后浮点可用 */

    h->priv              = PRIV_M;
    h->reservation_valid = 0;
    h->reservation_addr  = 0;
    h->mtimecmp          = 0;               /* 0 = 定时器关闭 */
    h->msip              = 0;               /* v0.13: 复位时没有软件中断 */
    h->waiting           = false;           /* v0.15: 复位后不在 WFI */
    memset(h->sb, 0, sizeof(h->sb));        /* v0.18: 写缓冲为空 */
    memset(h->lb, 0, sizeof(h->lb));        /* v0.19: 读缓冲为空 */
    h->lb_pending        = 0;
    h->state             = EMU_HALTED;
}

void emu_init_harts(emulator_t *emu, u32 nharts)
{
    u32 i;

    if (nharts == 0) nharts = 1;
    if (nharts > MAX_HARTS) nharts = MAX_HARTS;

    for (i = 0; i < nharts; i++) {
        hart_reset(&emu->harts[i], i);
    }
    for (i = nharts; i < MAX_HARTS; i++) {
        memset(&emu->harts[i], 0, sizeof(emu->harts[i]));
        emu->harts[i].state = EMU_HALTED;
    }

    /*
     * v0.14: PLIC 随机器一起复位。每个 hart 对应一个上下文
     * (本模拟器只有 M 态上下文), 所以 enable/threshold 一起清零。
     */
    plic_init(&emu->plic);

    emu->nharts = nharts;
    emu->cur    = 0;
}

void emu_init(emulator_t *emu, u32 mem_size)
{
    (void)mem_size;  /* 当前使用 memory.h 中的固定 MEM_SIZE */

    memory_init(&emu->mem);

    emu->mtime     = 0;
    emu->entry     = 0;
    emu->exit_code = 0;

    /* v0.18: 默认 SC, 写缓冲基础延迟 2 条指令 */
    emu->weak_mem  = false;
    emu->sb_lag    = 2;
    emu->lb_lag    = 0;         /* v0.19: 默认不模拟 load buffering */

    emu_init_harts(emu, 1);
}

void emu_free(emulator_t *emu)
{
    memory_free(&emu->mem);
}

/* =========================== ELF =========================== */

bool emu_load_elf(emulator_t *emu, const char *path)
{
    elf_info_t info;
    u32 entry = elf_load(path, &emu->mem, &info);
    u32 i;

    if (entry == 0) return false;

    emu->entry     = entry;
    emu->exit_code = 0;

    /* 所有 hart 从同一个入口开始执行 */
    for (i = 0; i < emu->nharts; i++) {
        emu->harts[i].rf.pc = entry;
        emu->harts[i].state = EMU_RUNNING;
    }

    printf("[EMU] pc set to entry = 0x%08X  (%u hart%s)\n",
           entry, emu->nharts, emu->nharts > 1 ? "s" : "");
    printf("[EMU] .text: [0x%08X — 0x%08X]\n", info.text_start, info.text_end);
    if (info.data_start != 0xFFFFFFFF) {
        printf("[EMU] .data: [0x%08X — 0x%08X]\n", info.data_start, info.data_end);
    }
    if (info.bss_start) {
        printf("[EMU] .bss:  [0x%08X — 0x%08X]\n", info.bss_start, info.bss_end);
    }
    return true;
}

/* =========================== 执行 =========================== */

u64 emu_run(emulator_t *emu, u64 max_steps)
{
    u64 steps = 0;
    decoded_inst_t inst;
    bool any;
    u32 i;

    /*
     * 轮转调度: 一轮里每个还活着的 hart 各执行一条指令。
     *
     * 真硬件是并行跑的, 但模拟器里所有 hart 串行执行, 指令之间
     * 严格按这个轮转顺序交错。对单 hart 程序完全没有影响; 对双核
     * 程序来说, 交错顺序是确定的, demo 因此可以重复验证。
     */
    do {
        for (i = 0; i < emu->nharts; i++) {
            if (emu->harts[i].state != EMU_RUNNING) continue;
            if (max_steps > 0 && steps >= max_steps) return steps;

            emu->cur = i;
            emu_step(emu, &inst);
            steps++;
        }

        any = false;
        for (i = 0; i < emu->nharts; i++) {
            if (emu->harts[i].state == EMU_RUNNING) {
                any = true;
                break;
            }
        }

        /*
         * v0.15: 所有活着的核都在 WFI 里。这时候再一轮轮空转只是
         * 浪费, 直接把 mtime 跳到最近的 mtimecmp; 如果没有核装过
         * 定时器, 也没有任何 PLIC 源能唤醒它们, 那就是真死锁 ——
         * 停机并报出来, 而不是让模拟器永远转下去。
         */
        if (any) {
            bool all_wait = true;
            bool have_timer = false;
            u64 next = 0;

            for (i = 0; i < emu->nharts; i++) {
                if (emu->harts[i].state != EMU_RUNNING) continue;
                if (!emu->harts[i].waiting) {
                    all_wait = false;
                    break;
                }
                /*
                 * v0.16: 两种定时器都算数 —— 机器定时器 (mtimecmp) 和
                 * S 态定时器 (Sstc 的 stimecmp)。谁先到期就快进到谁。
                 */
                {
                    const hart_t *h = &emu->harts[i];

                    if (h->mtimecmp != 0 && h->mtimecmp > emu->mtime &&
                        (!have_timer || h->mtimecmp < next)) {
                        next = h->mtimecmp;
                        have_timer = true;
                    }
                    if (h->csr.stimecmp != 0 && h->csr.stimecmp > emu->mtime &&
                        (!have_timer || h->csr.stimecmp < next)) {
                        next = h->csr.stimecmp;
                        have_timer = true;
                    }
                }
            }

            if (all_wait) {
                if (have_timer) {
                    emu->mtime = (next > emu->mtime) ? next - 1u : emu->mtime;
                } else {
                    /*
                     * 没有定时器不代表死锁: PLIC 可能刚被另一个核拉起来,
                     * 而等待中的核还没轮到执行、mip 还没同步。先看有没有
                     * 能唤醒的 PLIC 源; 真有才判定死锁。
                     */
                    bool have_ext = false;

                    for (i = 0; i < emu->nharts; i++) {
                        csr_t *csr;

                        if (emu->harts[i].state != EMU_RUNNING) continue;
                        csr = &emu->harts[i].csr;

                        /*
                         * 只有"使能了"的中断才能把 WFI 叫醒。光有 pending
                         * 源但对应使能位是 0, 一样是死锁。
                         */
                        if (plic_pending_for(&emu->plic, PLIC_CTX_M(i)) &&
                            (csr->mie & MIE_MEIE)) {
                            have_ext = true;
                            break;
                        }
                        if (plic_pending_for(&emu->plic, PLIC_CTX_S(i))) {
                            u32 seie = (csr->mideleg & MIP_SEIP)
                                     ? csr->sie : csr->mie;
                            if (seie & MIE_SEIE) {
                                have_ext = true;
                                break;
                            }
                        }
                    }

                    if (!have_ext) {
                        printf("[EMU] all harts in WFI with no interrupt source\n");
                        for (i = 0; i < emu->nharts; i++) {
                            if (emu->harts[i].state == EMU_RUNNING) {
                                emu->harts[i].state = EMU_HALTED;
                            }
                        }
                    }
                }
            }
        }
    } while (any && (max_steps == 0 || steps < max_steps));

    return steps;
}

bool emu_step(emulator_t *emu, decoded_inst_t *inst_out)
{
    hart_t *h = emu_hart(emu);
    u32 raw;
    bool compressed = false;
    u32 pc = h->rf.pc;

    /*
     * 0a. v0.15: 这条 hart 停在 WFI 里。
     *
     * 不取指, 只走一遍中断检查: 有中断就醒(下一轮正常执行/进 trap),
     * 没中断就消耗一个周期继续等。
     */
    if (h->waiting) {
        if (trap_check_interrupt(emu, pc)) {
            h->waiting = false;
            h->csr.mcycle++;
            return true;
        }
        if (trap_irq_pending(emu)) {
            h->waiting = false;
        }
        h->csr.mcycle++;
        return true;
    }

    /*
     * 0. 指令边界上检查定时器中断。
     *
     * 中断是异步的, 只能在两条指令之间进来。trap_check_interrupt
     * 顺便把全机器共享的 mtime 往前推一格。
     */
    if (trap_check_interrupt(emu, pc)) {
        hart_sb_flush(emu);     /* v0.18: 进 trap 前提交本 hart 的写 */
        hart_lb_drain(emu);     /* v0.19: 在途 load 也先写回 */
        h->csr.mcycle++;
        return true;            /* 这一"步"没有执行指令, 但跳进了 handler */
    }

    /* 1. Fetch: 取出 16-bit 或 32-bit 指令 (RV32C 允许 2 字节对齐) */
    if ((pc & 1u) != 0) {
        hart_lb_drain(emu);  /* v0.19: trap 前写回在途 load */
        if (trap_raise(emu, EXC_INST_MISALIGNED, pc, pc)) {
            h->csr.mcycle++;
            return true;
        }
        printf("[EMU] misaligned pc=0x%08X\n", pc);
        h->state = EMU_EXCEPTION;
        return false;
    }

    /*
     * v0.4: 取指也要过页表。
     * U 态执行到一页没有 X 权限的内存, 或者干脆没映射的地址, 就在这里
     * 报 12 (instruction page fault), mtval = 出错的 PC。
     */
    {
        u32 fetch_pa = 0;
        u16 lo = 0;
        u16 hi = 0;
        bool fetch_ok = true;
        int frc = mmu_translate(emu, pc, ACC_FETCH, &fetch_pa);

        if (frc != 0) {
            hart_lb_drain(emu);  /* v0.19: trap 前写回在途 load */
            if (trap_raise(emu, (u32)frc, pc, pc)) {
                h->csr.mcycle++;
                return true;
            }
            printf("[EMU] fetch page fault at pc=0x%08X\n", pc);
            h->state = EMU_EXCEPTION;
            return false;
        }

        /*
         * 先读 16 位。低两位不是 11 就是压缩指令 (RV32C),
         * 否则后面还跟着半个字。
         */
        if (!memory_read16(&emu->mem, fetch_pa, &lo)) {
            hart_lb_drain(emu);  /* v0.19: trap 前写回在途 load */
            if (trap_raise(emu, EXC_INST_ACCESS, pc, pc)) {
                h->csr.mcycle++;
                return true;
            }
            printf("[EMU] fetch failed at pc=0x%08X\n", pc);
            h->state = EMU_EXCEPTION;
            return false;
        }

        if ((lo & 3u) != 3u) {
            if (!expand_compressed(lo, &raw)) {
                hart_lb_drain(emu);  /* v0.19: trap 前写回在途 load */
                if (trap_raise(emu, EXC_ILLEGAL_INST, lo, pc)) {
                    h->csr.mcycle++;
                    return true;
                }
                printf("[EMU] bad compressed inst 0x%04X at pc=0x%08X\n", lo, pc);
                h->state = EMU_EXCEPTION;
                return false;
            }
            compressed = true;
        } else {
            /*
             * 32-bit 指令。正常情况下在同一个物理页里直接读 32 位;
             * 如果正好压在页尾 (只有 2 字节在本页), 把第二个半字
             * 单独翻译一次, 避免跨页时读到相邻物理页的内容。
             */
            if ((fetch_pa & 0xFFFu) <= 0xFFCu) {
                fetch_ok = memory_read32(&emu->mem, fetch_pa, &raw);
            } else {
                u32 pa2 = 0;
                if (mmu_translate(emu, pc + 2u, ACC_FETCH, &pa2) != 0 ||
                    !memory_read16(&emu->mem, pa2, &hi)) {
                    fetch_ok = false;
                } else {
                    raw = (u32)lo | ((u32)hi << 16);
                }
            }

            if (!fetch_ok) {
                hart_lb_drain(emu);  /* v0.19: trap 前写回在途 load */
                if (trap_raise(emu, EXC_INST_ACCESS, pc, pc)) {
                    h->csr.mcycle++;
                    return true;
                }
                printf("[EMU] fetch failed at pc=0x%08X\n", pc);
                h->state = EMU_EXCEPTION;
                return false;
            }
        }
    }

    /* 2. Decode */
    if (!decode(raw, inst_out)) {
        hart_lb_drain(emu);  /* v0.19: trap 前写回在途 load */
        if (trap_raise(emu, EXC_ILLEGAL_INST, raw, pc)) {
            h->csr.mcycle++;
            return true;
        }
        printf("[EMU] decode failed at pc=0x%08X raw=0x%08X\n", pc, raw);
        h->state = EMU_EXCEPTION;
        return false;
    }

    /* decode() 默认 len=4, 压缩指令在这里改回 2 (影响 PC 增量和链接值) */
    if (compressed) {
        inst_out->len = 2;
    }

    /* 3. Execute (内部更新 pc 和 state) */
    if (!execute(emu, inst_out)) {
        return false;
    }

    /* 4. 写缓冲/读缓冲老化: 这条指令退休后, 延迟减一 (v0.18/v0.19) */
    hart_sb_tick(emu);
    hart_lb_tick(emu);

    /* 5. 周期计数 */
    h->csr.mcycle++;

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

static u32 enc_b(s32 imm, u32 rs2, u32 rs1, u32 f3)
{
    u32 u = (u32)imm;
    return ((u >> 12) & 1u) << 31 | ((u >> 5) & 0x3Fu) << 25 |
           (rs2 & 0x1Fu) << 20 | (rs1 & 0x1Fu) << 15 |
           (f3 & 7u) << 12 | ((u >> 1) & 0xFu) << 8 |
           ((u >> 11) & 1u) << 7 | 0x63u;
}

static u32 enc_amo(u32 f5, u32 rs2, u32 rs1, u32 rd)
{
    return (f5 & 0x1Fu) << 27 | (rs2 & 0x1Fu) << 20 |
           (rs1 & 0x1Fu) << 15 | 2u << 12 | (rd & 0x1Fu) << 7 | 0x2Fu;
}

static void put32(emulator_t *emu, u32 addr, u32 val)
{
    memory_write32(&emu->mem, addr, val);
}

#define MH_CHECK(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", name); \
        fail++; \
    } else { \
        printf("  [OK]   %s\n", name); \
        pass++; \
    } \
} while (0)

void emu_selftest(void)
{
    emulator_t emu;
    decoded_inst_t inst;
    hart_t *h0, *h1;
    u32 v;
    int pass = 0, fail = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        Multi-Hart Self-Test (v0.12)                      ║\n");
    printf("║        mhartid / per-hart CSR / shared memory / LR-SC    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------- 1. 复位状态 ---------- */
    printf("── reset state ──\n");
    emu_init(&emu, MEM_SIZE);
    emu_init_harts(&emu, 2);
    h0 = &emu.harts[0];
    h1 = &emu.harts[1];

    MH_CHECK(emu.nharts == 2, "nharts = 2");
    MH_CHECK(h0->csr.mhartid == 0 && h1->csr.mhartid == 1,
             "mhartid = 0 / 1");
    MH_CHECK(h0->priv == PRIV_M && h1->priv == PRIV_M,
             "both harts reset to M-mode");
    MH_CHECK(h0->state == EMU_HALTED && h1->state == EMU_HALTED,
             "both harts start halted");

    /* ---------- 2. 读 mhartid CSR ---------- */
    printf("\n── csrr mhartid ──\n");
    /* csrrs x5, mhartid, x0 */
    put32(&emu, 0x100, enc_i((s32)CSR_MHARTID, 0, 2, 5, 0x73));

    h0->state = EMU_RUNNING;
    h0->rf.pc = 0x100;
    emu.cur = 0;
    emu_step(&emu, &inst);
    MH_CHECK(h0->rf.regs[5] == 0, "hart0 reads mhartid = 0");

    h1->state = EMU_RUNNING;
    h1->rf.pc = 0x100;
    emu.cur = 1;
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.regs[5] == 1, "hart1 reads mhartid = 1");

    /* ---------- 3. CSR 各自独立 ---------- */
    printf("\n── per-hart CSR ──\n");
    /* csrrw x0, mtvec, x1 */
    put32(&emu, 0x110, enc_i((s32)CSR_MTVEC, 1, 1, 0, 0x73));
    h1->rf.regs[1] = 0x800;
    h1->rf.pc = 0x110;
    emu.cur = 1;
    emu_step(&emu, &inst);
    MH_CHECK(h1->csr.mtvec == 0x800, "hart1 mtvec = 0x800");
    MH_CHECK(h0->csr.mtvec == 0, "hart0 mtvec untouched");

    /* ---------- 4. 共享内存 ---------- */
    printf("\n── shared memory ──\n");
    /* sw x2, 0(x1) —— hart0 写到 0x200 */
    put32(&emu, 0x120, enc_s(0, 2, 1, 2));
    h0->rf.regs[1] = 0x200;
    h0->rf.regs[2] = 0x12345678;
    h0->rf.pc = 0x120;
    emu.cur = 0;
    emu_step(&emu, &inst);

    /* lw x3, 0(x1) —— hart1 读同一个地址 */
    put32(&emu, 0x130, enc_i(0, 1, 2, 3, 0x03));
    h1->rf.regs[1] = 0x200;
    h1->rf.pc = 0x130;
    emu.cur = 1;
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.regs[3] == 0x12345678, "hart1 sees hart0's store");

    /* ---------- 5. 跨 hart LR/SC ---------- */
    printf("\n── cross-hart LR/SC ──\n");
    /* lr.w x2, (x1) */
    put32(&emu, 0x140, enc_amo(0x02, 0, 1, 2));
    /* sc.w x3, x2, (x1) */
    put32(&emu, 0x160, enc_amo(0x03, 2, 1, 3));
    /* sw x4, 0(x1) */
    put32(&emu, 0x150, enc_s(0, 4, 1, 2));

    h0->rf.regs[1] = 0x200;
    h0->rf.pc = 0x140;
    emu.cur = 0;
    emu_step(&emu, &inst);
    MH_CHECK(h0->reservation_valid && h0->reservation_addr == 0x200,
             "hart0 LR sets reservation");

    /* hart1 写到同一地址 → 保留失效 */
    h1->rf.regs[1] = 0x200;
    h1->rf.regs[4] = 99;
    h1->rf.pc = 0x150;
    emu.cur = 1;
    emu_step(&emu, &inst);
    MH_CHECK(h0->reservation_valid == 0,
             "hart1 store invalidates hart0 reservation");

    h0->rf.regs[2] = 7;
    h0->rf.pc = 0x160;
    emu.cur = 0;
    emu_step(&emu, &inst);
    MH_CHECK(h0->rf.regs[3] == 1, "SC fails after cross-hart write");
    memory_read32(&emu.mem, 0x200, &v);
    MH_CHECK(v == 99, "memory keeps hart1's value");

    /* 没有干扰时 SC 成功 */
    h0->rf.pc = 0x140;
    emu_step(&emu, &inst);          /* LR: x2 ← 内存里的 99 */
    h0->rf.regs[2] = 7;             /* 软件算出新值 */
    h0->rf.pc = 0x160;
    emu_step(&emu, &inst);          /* SC */
    MH_CHECK(h0->rf.regs[3] == 0, "SC succeeds when reservation intact");
    memory_read32(&emu.mem, 0x200, &v);
    MH_CHECK(v == 7, "memory now holds hart0's value");

    /* ---------- 6. 每个 hart 自己的 mtimecmp ---------- */
    printf("\n── per-hart mtimecmp ──\n");
    /* hart0 写 hart1 的比较值: lui x1, 0x02004; addi x1,x1,8; sw x2,0(x1) */
    put32(&emu, 0x170, 0x020040B7u);                    /* lui  x1, 0x02004 */
    put32(&emu, 0x174, enc_i(8, 1, 0, 1, 0x13));        /* addi x1, x1, 8  */
    put32(&emu, 0x178, enc_s(0, 2, 1, 2));              /* sw   x2, 0(x1)  */
    h0->rf.regs[2] = 123;
    h0->rf.pc = 0x170;
    emu.cur = 0;
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    MH_CHECK(h1->mtimecmp == 123, "writing 0x02004008 sets hart1's mtimecmp");
    MH_CHECK(h0->mtimecmp == 0, "hart0's mtimecmp untouched");

    /* ---------- 7. 轮转调度 + 原子计数 ---------- */
    printf("\n── round-robin: two harts count to 10 ──\n");
    emu_free(&emu);                 /* 换一台新机器, 先释放上一块内存 */
    emu_init(&emu, MEM_SIZE);
    emu_init_harts(&emu, 2);
    h0 = &emu.harts[0];
    h1 = &emu.harts[1];

    put32(&emu, 0x100, enc_i(0x200, 0, 0, 1, 0x13));    /* addi x1, x0, 0x200 */
    put32(&emu, 0x104, enc_i(5, 0, 0, 2, 0x13));        /* addi x2, x0, 5     */
    put32(&emu, 0x108, enc_i(1, 0, 0, 3, 0x13));        /* addi x3, x0, 1     */
    put32(&emu, 0x10C, enc_amo(0x00, 3, 1, 0));         /* amoadd.w x0,x3,(x1)*/
    put32(&emu, 0x110, enc_i(-1, 2, 0, 2, 0x13));       /* addi x2, x2, -1    */
    put32(&emu, 0x114, enc_b(-8, 0, 2, 1));             /* bnez x2, 0x10C     */
    put32(&emu, 0x118, 0x00000073u);                    /* ecall (exit)       */

    h0->rf.pc = 0x100; h0->state = EMU_RUNNING;
    h1->rf.pc = 0x100; h1->state = EMU_RUNNING;

    {
        u64 steps = emu_run(&emu, 0);
        memory_read32(&emu.mem, 0x200, &v);
        MH_CHECK(v == 10, "counter = 10 (5 + 5 atomic adds)");
        MH_CHECK(steps == 38, "19 instructions per hart, round-robin");
        MH_CHECK(h0->state == EMU_HALTED && h1->state == EMU_HALTED,
                 "both harts halted");
    }

    /* ---------- 8. CLINT msip / 软件中断 (v0.13) ---------- */
    printf("\n── CLINT msip / software interrupt ──\n");
    emu_free(&emu);
    emu_init(&emu, MEM_SIZE);
    emu_init_harts(&emu, 2);
    h0 = &emu.harts[0];
    h1 = &emu.harts[1];

    /*
     * hart0 写 0x02000004 —— 这是 hart1 的 msip, 相当于给它发一个 IPI。
     * lui x1, 0x02000; sw x2, 0(x1+4) 用 addi 调整地址。
     */
    put32(&emu, 0x100, 0x020000B7u);                 /* lui  x1, 0x02000   */
    put32(&emu, 0x104, enc_i(4, 1, 0, 1, 0x13));     /* addi x1, x1, 4     */
    put32(&emu, 0x108, enc_s(0, 2, 1, 2));           /* sw   x2, 0(x1)     */
    h0->rf.regs[2] = 1;
    h0->rf.pc = 0x100;
    h0->state = EMU_RUNNING;
    emu.cur = 0;
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    MH_CHECK(h1->msip == 1, "hart0 writes hart1 msip → pending");
    MH_CHECK((h1->csr.mip & MIP_MSIP) != 0, "hart1 mip.MSIP set by the write");

    /* hart1 从自己的 msip 地址读回 1 */
    put32(&emu, 0x110, enc_i(0, 1, 2, 3, 0x03));     /* lw x3, 0(x1)       */
    h1->rf.regs[1] = 0x02000004u;
    h1->rf.pc = 0x110;
    h1->state = EMU_RUNNING;
    emu.cur = 1;
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.regs[3] == 1, "reading msip returns the level");

    /* MIE=0 时只是 pending, 不陷入 */
    put32(&emu, 0x120, 0x00000013u);                 /* nop (addi x0,x0,0) */
    h1->csr.mtvec = 0x800;
    h1->csr.mie = MIE_MSIE;
    h1->csr.mstatus = 0;
    h1->rf.pc = 0x120;
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.pc == 0x124, "MIE=0 → interrupt stays pending");

    /* MIE=1 → 在指令边界进入 handler */
    h1->csr.mstatus = MSTATUS_MIE;
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.pc == 0x800, "MIE=1 → pc = mtvec");
    MH_CHECK(h1->csr.mcause == MCAUSE_IRQ_SOFT, "mcause = 0x80000003 (software)");
    MH_CHECK(h1->csr.mepc == 0x124, "mepc = interrupted instruction");
    MH_CHECK((h1->csr.mstatus & MSTATUS_MIE) == 0, "MIE cleared in handler");

    /*
     * handler 写 0 到自己的 msip: lui/addi 到 0x02000004 再 sw x0。
     * 电平信号不清就会反复进中断, 所以这一步是必须的。
     */
    put32(&emu, 0x800, 0x020000B7u);                 /* lui  x1, 0x02000   */
    put32(&emu, 0x804, enc_i(4, 1, 0, 1, 0x13));     /* addi x1, x1, 4     */
    put32(&emu, 0x808, enc_s(0, 0, 1, 2));           /* sw   x0, 0(x1)     */
    h1->rf.pc = 0x800;
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    emu_step(&emu, &inst);
    MH_CHECK(h1->msip == 0, "writing 0 clears msip");
    MH_CHECK((h1->csr.mip & MIP_MSIP) == 0, "mip.MSIP drops with msip");

    /* mret 回到被中断的指令, 没有新 pending 就不再进 handler */
    put32(&emu, 0x80C, 0x30200073u);                 /* mret               */
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.pc == 0x124, "mret returns to mepc");

    /* 电平语义: 不写 0 的话, 一 mret 就再进一次 */
    h1->msip = 1;
    h1->csr.mip |= MIP_MSIP;
    h1->csr.mstatus = MSTATUS_MIE;
    put32(&emu, 0x124, 0x00000013u);
    h1->rf.pc = 0x124;
    emu_step(&emu, &inst);
    MH_CHECK(h1->rf.pc == 0x800, "uncleared msip retriggers immediately");

    /* 优先级: 软件中断先于定时器中断 */
    h1->csr.mstatus = MSTATUS_MIE;
    h1->csr.mie = MIE_MSIE | MIE_MTIE;
    h1->mtimecmp = 1;                    /* mtime 早已超过 → MTIP pending */
    h1->rf.pc = 0x124;
    emu_step(&emu, &inst);
    MH_CHECK(h1->csr.mcause == MCAUSE_IRQ_SOFT,
             "MSIP has priority over MTIP");
    h1->msip = 0;
    h1->csr.mip &= ~MIP_MSIP;
    h1->csr.mstatus = MSTATUS_MIE;
    h1->rf.pc = 0x124;
    emu_step(&emu, &inst);
    MH_CHECK(h1->csr.mcause == MCAUSE_IRQ_TIMER,
             "timer fires after software IRQ is cleared");

    printf("\n  Result: %d passed, %d failed\n", pass, fail);
    emu_free(&emu);
}

void emu_dump_state(emulator_t *emu)
{
    u32 i;

    printf("\n=== Emulator State (%u hart%s) ===\n",
           emu->nharts, emu->nharts > 1 ? "s" : "");
    printf("  mtime = %llu\n", (unsigned long long)emu->mtime);

    for (i = 0; i < emu->nharts; i++) {
        hart_t *h = &emu->harts[i];

        printf("\n--- hart %u ---\n", i);
        printf("  state = %d\n", h->state);
        printf("  pc    = 0x%08X\n", h->rf.pc);
        if (h->state == EMU_HALTED) {
            printf("  exit_code = %u\n", emu->exit_code);
        }
        regfile_dump(&h->rf);
        regfile_dump_f(&h->rf);
        csr_dump(&h->csr);
    }
}
