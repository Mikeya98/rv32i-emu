/**
 * @file    emu.h
 * @brief   RV32I Emulator — 顶层模拟器状态
 *
 * v0.12 起支持多 hart: 一台机器 (emulator_t) 里有 N 个核 (hart_t),
 * 它们共享同一块物理内存, 但各自有寄存器、CSR、特权级和 LR/SC 保留集。
 * v0.13 补上 CLINT msip: 核间可以互相发软件中断 (IPI)。
 * v0.14 补上 PLIC: 外部设备中断按优先级/阈值分发给各个核。
 */

#ifndef _RVEMU_EMU_H
#define _RVEMU_EMU_H

#include "types.h"
#include "memory.h"
#include "regfile.h"
#include "csr.h"
#include "decode.h"
#include "plic.h"

#define ELF_ENTRY_MAX   256   /* 入口地址 (运行时设置) */
#define MAX_HARTS       4     /* 最多模拟几个核 */
#define SB_ENTRIES      8     /* 每个 hart 的写缓冲项数 */

typedef enum {
    EMU_RUNNING,
    EMU_HALTED,         /* ebreak / ecall + stop */
    EMU_EXCEPTION,      /* 未处理异常 */
} emu_state_t;

/**
 * @brief 写缓冲里的一条未提交 store (v0.18)
 *
 * 弱内存模型下, store 先进入本 hart 的写缓冲, 过若干条指令才写进
 * 共享内存。delay 归零之前, 别的 hart 看不到这个值, 本 hart 自己
 * 的 load 可以从缓冲里直接转发。
 */
typedef struct {
    u32  addr;          /* 物理地址 */
    u64  value;         /* 写入值 (最多 8 字节) */
    u32  size;          /* 1/2/4/8 */
    u32  delay;         /* 还剩几条指令变成全局可见 */
    bool valid;
} sb_entry_t;

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

    /*
     * v0.13: CLINT 的 msip[hartid]。别的核往这个地址写 1, 本核就挂上
     * 软件中断 (mip.MSIP); 写 0 才撤掉。电平语义, 和 mtimecmp 一样
     * 由发中断的一方负责清除。
     */
    u32         msip;

    /*
     * v0.15: WFI 停在这里。waiting=1 时这条 hart 不再取指, 每个
     * 指令边界只推进 mtime、检查中断; 中断到了就自动醒来接着跑。
     */
    bool        waiting;

    /* v0.18: 写缓冲。SC 模型下始终为空。 */
    sb_entry_t  sb[SB_ENTRIES];

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

    /* v0.14: 外部中断控制器 (所有 hart 共享, 每个 hart 一个上下文) */
    plic_t      plic;

    u32         entry;              /* ELF 入口地址 (所有 hart 相同) */
    u32         exit_code;          /* 最后一个退出 hart 的退出码 */

    /*
     * v0.18 内存模型:
     *   weak_mem = false  → 顺序一致 (SC), store 立刻可见
     *   weak_mem = true   → 每个 hart 一个写缓冲, store 延迟可见
     * sb_lag 是基础延迟 (条指令), 再叠加按地址计算的 bank 偏斜。
     */
    bool        weak_mem;
    u32         sb_lag;
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

/* --- 写缓冲 / 弱内存模型 (v0.18) --- */

/* 把当前 hart 的写缓冲全部提交到内存 */
void  hart_sb_flush(emulator_t *emu);
/* 提交所有 hart 的写缓冲 (原子指令用) */
void  hart_sb_flush_all(emulator_t *emu);
/* 每条指令退休后调用: 递减延迟, 到期的写进内存 */
void  hart_sb_tick(emulator_t *emu);
/* 经写缓冲的 store / load (大小 1/2/4/8) */
bool  hart_mem_store(emulator_t *emu, u32 pa, u64 value, u32 size);
bool  hart_mem_load(emulator_t *emu, u32 pa, u64 *value, u32 size);

/* 写缓冲自测 */
void  membuf_selftest(void);

#endif /* _RVEMU_EMU_H */
