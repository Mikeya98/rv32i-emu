/**
 * @file    mmu.c
 * @brief   RV32I Emulator — Sv32 分页实现 (v0.4)
 *
 * 一次翻译就是两次读内存:
 *
 *   satp.PPN ──→ 一级表 ──[VPN1]──→ 二级表 ──[VPN0]──→ 物理页 ──[offset]
 *
 * 每一步都要检查 V (有效)、权限位和 U 位。任何一步不过, 就按访问
 * 类型报 12 / 13 / 15 三种缺页异常。
 */

#include "mmu.h"
#include "memory.h"
#include <stdio.h>

/* =========================== 辅助 =========================== */

bool mmu_read_pte(emulator_t *emu, u32 pte_addr, u32 *pte)
{
    return memory_read32(&emu->mem, pte_addr, pte);
}

/** 访问类型 → 缺页异常码 */
static int fault_cause(access_t acc)
{
    switch (acc) {
    case ACC_FETCH: return EXC_INST_PAGE_FAULT;
    case ACC_STORE: return EXC_STORE_PAGE_FAULT;
    default:        return EXC_LOAD_PAGE_FAULT;
    }
}

/**
 * @brief  检查一个叶子 PTE 是否允许这次访问
 * @return true = 允许
 */
static bool pte_allows(u32 pte, access_t acc, u32 priv)
{
    /* 叶子项至少要有 R 或 X; W=1 而 R=0 是保留编码 */
    if ((pte & (PTE_R | PTE_X)) == 0) return false;
    if ((pte & PTE_W) && !(pte & PTE_R)) return false;

    /* U 态只能访问 PTE.U = 1 的页; M 态不做翻译, 走不到这里 */
    if (priv == PRIV_U && !(pte & PTE_U)) return false;

    switch (acc) {
    case ACC_FETCH:
        return (pte & PTE_X) != 0;
    case ACC_LOAD:
        return (pte & PTE_R) != 0;
    case ACC_STORE:
        return (pte & PTE_R) != 0 && (pte & PTE_W) != 0;
    }
    return false;
}

/** 自动置 A / D 位 (硬件更新) */
static void pte_update_ad(emulator_t *emu, u32 pte_addr, u32 pte, access_t acc)
{
    u32 new_pte = pte | PTE_A;

    if (acc == ACC_STORE) {
        new_pte |= PTE_D;
    }
    if (new_pte != pte) {
        memory_write32(&emu->mem, pte_addr, new_pte);
    }
}

/* =========================== 翻译 =========================== */

int mmu_translate(emulator_t *emu, u32 va, access_t acc, u32 *pa)
{
    const csr_t *csr = &emu->csr;
    u32 satp = csr->satp;
    u32 vpn1, vpn0;
    u32 pte_addr, pte;
    u32 table;

    /*
     * M 态或 bare 模式: 虚拟地址就是物理地址。
     *
     * 真实硬件在 M 态还看 mstatus.MPRV 决定要不要翻译, 本模拟器
     * 没有实现 S 态和 MPRV, 所以 M 态一律直通 —— 内核代码在 M 态
     * 里操作页表, 用的必须是物理地址, 这一点和真机一致。
     */
    if (emu->priv == PRIV_M || (satp & SATP_MODE_BIT) == 0) {
        *pa = va;
        return 0;
    }

    vpn1 = (va >> 22) & 0x3FFu;
    vpn0 = (va >> 12) & 0x3FFu;

    /* 一级表: 根表物理地址 = satp.PPN << 12 */
    table = (satp & SATP_PPN_MASK) << 12;
    pte_addr = table + vpn1 * 4u;

    if (!mmu_read_pte(emu, pte_addr, &pte)) {
        return fault_cause(acc);
    }
    if ((pte & PTE_V) == 0) {
        return fault_cause(acc);
    }

    /*
     * 叶子项判定: R 或 X 非零就是叶子。
     * 一级叶子是 4MB 大页, 要求 PPN[0] = 0, 否则是"未对齐大页",
     * 规范要求报缺页而不是猜一个地址。
     */
    if ((pte & (PTE_R | PTE_X)) != 0) {
        if (((pte >> PTE_PPN_SHIFT) & 0x3FFu) != 0) {
            return fault_cause(acc);        /* 大页 PPN[0] 非 0, 属于未对齐大页 */
        }
        if (!pte_allows(pte, acc, emu->priv)) {
            return fault_cause(acc);
        }
        pte_update_ad(emu, pte_addr, pte, acc);
        *pa = PTE_PA(pte & ~0x3FFu) | (va & 0x3FFFFFu);
        return 0;
    }

    /* 非叶子: 下一级表的物理地址 */
    table = PTE_PA(pte);
    pte_addr = table + vpn0 * 4u;

    if (!mmu_read_pte(emu, pte_addr, &pte)) {
        return fault_cause(acc);
    }
    if ((pte & PTE_V) == 0) {
        return fault_cause(acc);
    }
    if ((pte & (PTE_R | PTE_X)) == 0) {
        return fault_cause(acc);            /* 二级表里不允许再有指针 */
    }
    if (!pte_allows(pte, acc, emu->priv)) {
        return fault_cause(acc);
    }

    pte_update_ad(emu, pte_addr, pte, acc);
    *pa = PTE_PA(pte) | (va & 0xFFFu);
    return 0;
}

/* =========================== 自测 =========================== */

static int mmu_pass = 0;
static int mmu_fail = 0;

#define MMU_CHECK(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", name); \
        mmu_fail++; \
    } else { \
        printf("  [OK]   %s\n", name); \
        mmu_pass++; \
    } \
} while (0)

/* 测试用的内存布局 (都在 1MB 物理内存里) */
#define T_ROOT      0x70000u       /* 一级表 */
#define T_L2        0x71000u       /* 二级表 */
#define T_L2B       0x72000u       /* 第二个二级表 (演示另一个 4MB 区域) */
#define T_DATA      0x73000u       /* 数据页 */
#define T_CODE      0x74000u       /* 代码页 */
#define T_RO        0x75000u       /* 只读页 */

/* 虚拟地址 */
#define VA_CODE     0x00010000u    /* 与物理地址相同, 方便验证 identity 映射 */
#define VA_DATA     0x40000000u
#define VA_DATA2    0x40001000u
#define VA_RO       0x40002000u
#define VA_BIG      0xC0000000u
#define VA_NOMAP    0x50000000u

static u32 make_pte(u32 pa, u32 flags)
{
    return ((pa >> 12) << PTE_PPN_SHIFT) | flags;
}

/* 建一张测试页表: 代码页 + 数据页 + 只读页 + 一个 4MB 大页 */
static void build_test_tables(emulator_t *emu)
{
    u32 i;

    for (i = 0; i < 1024; i++) {
        memory_write32(&emu->mem, T_ROOT + i * 4u, 0);
        memory_write32(&emu->mem, T_L2 + i * 4u, 0);
        memory_write32(&emu->mem, T_L2B + i * 4u, 0);
    }

    /* VA 0x00000000-0x003FFFFF → T_L2 (VPN1 = 0) */
    memory_write32(&emu->mem, T_ROOT + 0 * 4u,
                   make_pte(T_L2, PTE_V));
    /* VA 0x40000000-0x403FFFFF → T_L2B (VPN1 = 0x100) */
    memory_write32(&emu->mem, T_ROOT + 0x100 * 4u,
                   make_pte(T_L2B, PTE_V));

    /* 4KB 页: 代码 (X|R), 数据 (R|W), 只读 (R) —— 全部 U 可访问 */
    memory_write32(&emu->mem, T_L2 + ((VA_CODE >> 12) & 0x3FFu) * 4u,
                   make_pte(T_CODE, PTE_V | PTE_R | PTE_X | PTE_U));
    memory_write32(&emu->mem, T_L2B + 0 * 4u,
                   make_pte(T_DATA, PTE_V | PTE_R | PTE_W | PTE_U));
    memory_write32(&emu->mem, T_L2B + 1 * 4u,
                   make_pte(T_DATA, PTE_V | PTE_R | PTE_W | PTE_U));
    memory_write32(&emu->mem, T_L2B + 2 * 4u,
                   make_pte(T_RO, PTE_V | PTE_R | PTE_U));

    /* 4MB 大页: VA 0xC0000000 → PA 0x00000000 (PPN[0] = 0) */
    memory_write32(&emu->mem, T_ROOT + 0x300 * 4u,
                   ((0u >> 12) << PTE_PPN_SHIFT) |
                   PTE_V | PTE_R | PTE_W | PTE_X | PTE_U);

    /* VA_NOMAP 不填, 保持无效 */
}

/* csrrs rd, csr, x0  —— 自测里手工编码 CSR 指令 */
static u32 enc_csrrs(u32 rd, u32 csr)
{
    return (csr << 20) | (0u << 15) | (2u << 12) | ((rd & 0x1Fu) << 7) | 0x73u;
}

/* 准备一个 U 态模拟器: 页表建好, satp 打开 */
static void setup_user_emu(emulator_t *emu)
{
    emu_init(emu, MEM_SIZE);
    emu->state = EMU_RUNNING;
    emu->priv  = PRIV_U;
    build_test_tables(emu);
    emu->csr.satp = SATP_MODE_BIT | (T_ROOT >> 12);
}

void mmu_selftest(void)
{
    emulator_t emu;
    u32 pa;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        Sv32 Paging + Privilege Self-Test                 ║\n");
    printf("║        4KB page / 4MB superpage / U bit / A-D bits       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------- 1. M 态直通 ---------- */
    printf("── M-mode bypasses translation ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.priv = PRIV_M;
    emu.csr.satp = SATP_MODE_BIT | (T_ROOT >> 12);
    MMU_CHECK(mmu_translate(&emu, VA_DATA, ACC_LOAD, &pa) == 0 &&
              pa == VA_DATA, "M-mode: satp ignored, VA == PA");
    MMU_CHECK(mmu_translate(&emu, VA_NOMAP, ACC_STORE, &pa) == 0,
              "M-mode: unmapped VA still accessible");

    /* ---------- 2. bare 模式 ---------- */
    printf("\n── bare mode (satp.MODE = 0) ──\n");
    emu.priv = PRIV_U;
    emu.csr.satp = 0;
    MMU_CHECK(mmu_translate(&emu, 0x12345000u, ACC_LOAD, &pa) == 0 &&
              pa == 0x12345000u, "U-mode bare: VA == PA");

    /* ---------- 3. 4KB 页翻译 ---------- */
    printf("\n── Sv32 4KB page ──\n");
    setup_user_emu(&emu);
    MMU_CHECK(mmu_translate(&emu, VA_DATA + 0x234u, ACC_LOAD, &pa) == 0 &&
              pa == T_DATA + 0x234u, "load: VA 0x40000234 → PA 0x730234");
    MMU_CHECK(mmu_translate(&emu, VA_DATA2, ACC_STORE, &pa) == 0 &&
              pa == T_DATA, "store: second page of the same table");
    MMU_CHECK(mmu_translate(&emu, VA_CODE, ACC_FETCH, &pa) == 0 &&
              pa == T_CODE, "fetch: X page");

    /* ---------- 4. A / D 位自动置位 ---------- */
    printf("\n── A/D bits ──\n");
    {
        u32 pte;
        u32 addr = T_L2B + 0 * 4u;
        memory_read32(&emu.mem, addr, &pte);
        MMU_CHECK((pte & PTE_A) != 0, "A set after load");
        MMU_CHECK((pte & PTE_D) == 0, "D still 0 after load only");

        mmu_translate(&emu, VA_DATA, ACC_STORE, &pa);
        memory_read32(&emu.mem, addr, &pte);
        MMU_CHECK((pte & PTE_D) != 0, "D set after store");
    }

    /* ---------- 5. 权限: 只读页不能写 ---------- */
    printf("\n── permissions ──\n");
    MMU_CHECK(mmu_translate(&emu, VA_RO, ACC_LOAD, &pa) == 0,
              "read-only page: load allowed");
    MMU_CHECK(mmu_translate(&emu, VA_RO, ACC_STORE, &pa) == EXC_STORE_PAGE_FAULT,
              "read-only page: store → cause 15");
    MMU_CHECK(mmu_translate(&emu, VA_DATA, ACC_FETCH, &pa) == EXC_INST_PAGE_FAULT,
              "data page (no X): fetch → cause 12");

    /* ---------- 6. U 位 ---------- */
    printf("\n── U bit ──\n");
    {
        u32 pte;
        /* 把数据页的 U 位清掉, U 态就不该再能访问 */
        memory_read32(&emu.mem, T_L2B + 0 * 4u, &pte);
        memory_write32(&emu.mem, T_L2B + 0 * 4u, pte & ~PTE_U);
        MMU_CHECK(mmu_translate(&emu, VA_DATA, ACC_LOAD, &pa) == EXC_LOAD_PAGE_FAULT,
                  "PTE.U = 0 → U-mode load fault");
        memory_write32(&emu.mem, T_L2B + 0 * 4u, pte);
    }

    /* ---------- 7. 未映射地址 ---------- */
    printf("\n── unmapped ──\n");
    MMU_CHECK(mmu_translate(&emu, VA_NOMAP, ACC_LOAD, &pa) == EXC_LOAD_PAGE_FAULT,
              "unmapped VPN1 → load fault (13)");
    MMU_CHECK(mmu_translate(&emu, 0x40100000u, ACC_STORE, &pa) == EXC_STORE_PAGE_FAULT,
              "invalid second-level entry → store fault (15)");

    /* ---------- 8. 4MB 大页 ---------- */
    printf("\n── 4MB superpage ──\n");
    MMU_CHECK(mmu_translate(&emu, VA_BIG + 0x124u, ACC_LOAD, &pa) == 0 &&
              pa == 0x124u, "superpage: 0xC0000124 → 0x00000124");
    MMU_CHECK(mmu_translate(&emu, VA_BIG + 0x100000u, ACC_STORE, &pa) == 0 &&
              pa == 0x100000u, "superpage covers 4MB");

    /* ---------- 9. 未对齐大页 ---------- */
    printf("\n── misaligned superpage ──\n");
    {
        u32 pte;
        memory_read32(&emu.mem, T_ROOT + 0x300 * 4u, &pte);
        memory_write32(&emu.mem, T_ROOT + 0x300 * 4u, pte | (1u << PTE_PPN_SHIFT));
        MMU_CHECK(mmu_translate(&emu, VA_BIG, ACC_LOAD, &pa) == EXC_LOAD_PAGE_FAULT,
                  "PPN[0] != 0 → fault");
        memory_write32(&emu.mem, T_ROOT + 0x300 * 4u, pte);
    }

    /* ---------- 10. ecall 从 U 态 / mret 到 U 态 ---------- */
    printf("\n── privilege transitions ──\n");
    {
        emulator_t e2;
        decoded_inst_t inst;

        emu_init(&e2, MEM_SIZE);
        e2.state = EMU_RUNNING;
        e2.csr.mtvec = 0x300;
        e2.priv = PRIV_U;
        /* 0x40: ecall */
        memory_write32(&e2.mem, 0x40, 0x00000073);
        e2.rf.pc = 0x40;
        emu_step(&e2, &inst);
        MMU_CHECK(e2.csr.mcause == EXC_ECALL_U, "ecall from U → cause 8");
        MMU_CHECK(((e2.csr.mstatus & MSTATUS_MPP_MASK) >> 11) == PRIV_U,
                  "MPP records U-mode");
        MMU_CHECK(e2.priv == PRIV_M, "trap enters M-mode");

        /* handler 里 mret, MPP 还是 U → 回到 U 态 */
        memory_write32(&e2.mem, 0x300, 0x30200073);   /* mret */
        e2.rf.pc = 0x300;
        emu_step(&e2, &inst);
        MMU_CHECK(e2.priv == PRIV_U, "mret returns to U-mode");
        MMU_CHECK(e2.rf.pc == 0x40, "pc = mepc");
        MMU_CHECK(((e2.csr.mstatus & MSTATUS_MPP_MASK) >> 11) == PRIV_U,
                  "MPP reset to U (least privileged supported)");
    }

    /* ---------- 11. U 态执行特权指令 ---------- */
    printf("\n── privileged instructions in U-mode ──\n");
    {
        emulator_t e2;
        decoded_inst_t inst;

        emu_init(&e2, MEM_SIZE);
        e2.state = EMU_RUNNING;
        e2.csr.mtvec = 0x300;
        e2.priv = PRIV_U;
        /* 0x40: csrr t0, mstatus (0x300)  → csrrs x5, 0x300, x0 */
        memory_write32(&e2.mem, 0x40, enc_csrrs(5, CSR_MSTATUS));
        e2.rf.pc = 0x40;
        emu_step(&e2, &inst);
        MMU_CHECK(e2.csr.mcause == EXC_ILLEGAL_INST,
                  "CSR access from U → illegal instruction");
        MMU_CHECK(e2.rf.pc == 0x300, "pc = mtvec");

        /* 0x50: mret —— U 态执行也是非法指令 */
        emu_init(&e2, MEM_SIZE);
        e2.state = EMU_RUNNING;
        e2.csr.mtvec = 0x300;
        e2.priv = PRIV_U;
        memory_write32(&e2.mem, 0x50, 0x30200073);
        e2.rf.pc = 0x50;
        emu_step(&e2, &inst);
        MMU_CHECK(e2.csr.mcause == EXC_ILLEGAL_INST, "mret from U → illegal");
    }

    printf("\n  Result: %d passed, %d failed\n", mmu_pass, mmu_fail);
}
