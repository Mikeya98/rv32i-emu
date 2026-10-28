/**
 * @file    mmu.h
 * @brief   RV32I Emulator — Sv32 分页 (v0.4)
 *
 * Sv32 是 RV32 的唯一一种页表格式:
 *
 *   虚拟地址 32 位 = VPN[1] (10 位) | VPN[0] (10 位) | offset (12 位)
 *   页表项   32 位 = PPN (22 位) | 保留位 | D A G U X W R V
 *
 *   一级表 (4KB, 1024 项) 覆盖 4MB
 *   二级表 (4KB, 1024 项) 覆盖 4KB
 *
 * 一级页表项如果 R/X 全为 0, 是"指向下一级表"的指针; 否则是叶子项:
 *   - 一级叶子 → 4MB 大页 (PPN[0] 必须为 0)
 *   - 二级叶子 → 4KB 页
 *
 * 特权级规则 (本模拟器只实现 M/U, 不做 MPRV):
 *   - M 态访存一律不翻译, 直接用物理地址;
 *   - U 态走 satp: MODE=0 是 bare (物理地址 = 虚拟地址),
 *     MODE=1 是 Sv32 页表翻译。
 *
 * 权限:
 *   取指要 X, 读要 R, 写要 W; U 态还要求 PTE.U = 1。
 *   A (accessed) / D (dirty) 由硬件自动置位 —— 真实硬件允许选择
 *   "置位"或"缺页让软件填", 这里选前者, 少一条异常路径。
 */

#ifndef _RVEMU_MMU_H
#define _RVEMU_MMU_H

#include "types.h"
#include "emu.h"

/* satp 的 MODE / PPN 位定义在 csr.h (CSR 模块负责读写) */

/* PTE 位 */
#define PTE_V   (1u << 0)   /* valid */
#define PTE_R   (1u << 1)   /* readable */
#define PTE_W   (1u << 2)   /* writable */
#define PTE_X   (1u << 3)   /* executable */
#define PTE_U   (1u << 4)   /* user accessible */
#define PTE_G   (1u << 5)   /* global */
#define PTE_A   (1u << 6)   /* accessed */
#define PTE_D   (1u << 7)   /* dirty */

#define PTE_PPN_SHIFT   10u
#define PTE_PA(pte)     (((pte) >> PTE_PPN_SHIFT) << 12)

/* 访问类型 */
typedef enum {
    ACC_FETCH = 0,
    ACC_LOAD  = 1,
    ACC_STORE = 2
} access_t;

/**
 * @brief  把虚拟地址翻译成物理地址
 *
 * @param  emu  模拟器 (读 satp / priv, 读取页表用物理内存)
 * @param  va   虚拟地址
 * @param  acc  访问类型 (取指/读/写)
 * @param  pa   输出物理地址
 * @return 0 成功; 否则返回异常码 (12 取指缺页 / 13 读缺页 / 15 写缺页)
 *
 * 失败时不做任何状态修改, 由调用者负责 trap_raise —— 这样翻译函数
 * 保持纯粹, 也能被自测直接调用。
 */
int  mmu_translate(emulator_t *emu, u32 va, access_t acc, u32 *pa);

/** 读取一个 PTE (物理地址寻址, 越界返回 false) */
bool mmu_read_pte(emulator_t *emu, u32 pte_addr, u32 *pte);

/** Sv32 / 特权级 自测 */
void mmu_selftest(void);

#endif /* _RVEMU_MMU_H */
