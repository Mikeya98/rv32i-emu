/**
 * @file    emu.h
 * @brief   RV32I Emulator — 顶层模拟器状态
 *
 * 包含 memory + regfile + csr + 运行状态。
 */

#ifndef _RVEMU_EMU_H
#define _RVEMU_EMU_H

#include "types.h"
#include "memory.h"
#include "regfile.h"
#include "csr.h"

#define ELF_ENTRY_MAX   256   /* 入口地址 (运行时设置) */

typedef enum {
    EMU_RUNNING,
    EMU_HALTED,         /* ebreak / ecall + stop */
    EMU_EXCEPTION,      /* 未处理异常 */
} emu_state_t;

typedef struct {
    memory_t    mem;
    regfile_t   rf;
    csr_t       csr;

    emu_state_t state;
    u32         entry;         /* ELF 入口地址 */
    u32         exit_code;     /* ecall 退出码 */
} emulator_t;

/* --- API --- */

void  emu_init(emulator_t *emu, u32 mem_size);
void  emu_free(emulator_t *emu);

/* 加载 ELF 到内存, 设置 entry */
bool  emu_load_elf(emulator_t *emu, const char *path);

/* 执行 N 条指令 (0 = 无限), 返回实际执行数 */
u64   emu_run(emulator_t *emu, u64 max_steps);

/* 单步执行一条指令, 返回解码信息 */
bool  emu_step(emulator_t *emu, decoded_inst_t *inst_out);

/* 打印模拟器状态 */
void  emu_dump_state(emulator_t *emu);

#endif /* _RVEMU_EMU_H */
