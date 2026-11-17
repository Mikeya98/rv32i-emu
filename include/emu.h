/**
 * @file    emu.h
 * @brief   RV32I Emulator — 顶层模拟器状态
 *
 * v0.12 起支持多 hart: 一台机器 (emulator_t) 里有 N 个核 (hart_t),
 * 它们共享同一块物理内存, 但各自有寄存器、CSR、特权级和 LR/SC 保留集。
 */

#ifndef _RVEMU_EMU_H
#define _RVEMU_EMU_H

#include "types.h"
#include "memory.h"
#include "regfile.h"
#include "csr.h"
#include "decode.h"

#define ELF_ENTRY_MAX   256   /* 入口地址 (运行时设置) */
#define MAX_HARTS       4     /* 最多模拟几个核 */

typedef enum {
    EMU_RUNNING,
    EMU_HALTED,         /* ebreak / ecall + stop */
    EMU_EXCEPTION,      /* 未处理异常 */
} emu_state_t;

/**
 * @brief 一个 hart (硬件线程) 的完整现场
 *
 * 搬到真实硬件上, 这些就是每个核各自的那一份:
 *   - 32 个整数寄存器 + 32 个浮点寄存器
 *   - 全部 CSR
 *   - 当前特权级
 *   - LR/SC 保留地址 (保留集是 per-hart 的)
 *   - mtimecmp (CLINT 里每个 hart 一个)
 */
typedef struct {
    regfile_t   rf;
    csr_t       csr;

    /* 当前特权级 (v0.4): PRIV_M = 3 复位值, PRIV_U = 0。 */
    u32         priv;

    /* RV32A: LR/SC 保留集。任何 hart 写到这个地址, 保留都会失效。 */
    u32         reservation_valid;
    u32         reservation_addr;

    /* 本 hart 的定时器比较值; 0 表示关闭 */
    u64         mtimecmp;

    emu_state_t state;
} hart_t;

/**
 * @brief 一台机器: 共享内存 + 若干 hart
 */
typedef struct {
    memory_t    mem;                /* 所有 hart 共享的物理内存 */

    hart_t      harts[MAX_HARTS];
    u32         nharts;             /* 实际启用的核数 */
    u32         cur;                /* 当前正在执行哪个 hart */

    /* CLINT 的 mtime 是全机器共享的: 每执行一条指令 +1,
     * 所有 hart 看到的是同一个时间轴。 */
    u64         mtime;

    u32         entry;              /* ELF 入口地址 (所有 hart 相同) */
    u32         exit_code;          /* 最后一个退出 hart 的退出码 */
} emulator_t;

/** 取当前 hart 的可写现场 */
static inline hart_t *emu_hart(emulator_t *emu)
{
    return &emu->harts[emu->cur];
}

/** 取当前 hart 的只读现场 */
static inline const hart_t *emu_hart_c(const emulator_t *emu)
{
    return &emu->harts[emu->cur];
}

/* --- API --- */

void  emu_init(emulator_t *emu, u32 mem_size);

/**
 * @brief 启用多个 hart
 *
 * 必须在 emu_init 之后、emu_load_elf 之前调用。所有 hart 复位到
 * M 态, 加载 ELF 后从同一个入口开始执行 —— 真实硬件也是这样,
 * 固件靠 mhartid 决定谁是启动核。
 */
void  emu_init_harts(emulator_t *emu, u32 nharts);

void  emu_free(emulator_t *emu);

/* 加载 ELF 到内存, 设置 entry (所有 hart 的 pc) */
bool  emu_load_elf(emulator_t *emu, const char *path);

/* 轮流执行各 hart, 直到全部停机 / 达到步数上限 */
u64   emu_run(emulator_t *emu, u64 max_steps);

/* 单步执行当前 hart 的一条指令 */
bool  emu_step(emulator_t *emu, decoded_inst_t *inst_out);

/* 打印模拟器状态 */
void  emu_dump_state(emulator_t *emu);

/* 多 hart 自测 (v0.12) */
void  emu_selftest(void);

#endif /* _RVEMU_EMU_H */
