/**
 * @file    emu.c
 * @brief   RV32I Emulator — 顶层模拟器实现
 *
 * fetch-decode-execute 主循环。
 */

#include "emu.h"
#include "elf.h"
#include "execute.h"
#include <stdio.h>
#include <string.h>

void emu_init(emulator_t *emu, u32 mem_size)
{
    (void)mem_size;  /* 当前使用 memory.h 中的固定 MEM_SIZE */

    memory_init(&emu->mem);
    regfile_init(&emu->rf);
    csr_init(&emu->csr);

    emu->state     = EMU_HALTED;
    emu->entry     = 0;
    emu->exit_code = 0;
}

void emu_free(emulator_t *emu)
{
    memory_free(&emu->mem);
}

bool emu_load_elf(emulator_t *emu, const char *path)
{
    elf_info_t info;
    u32 entry = elf_load(path, &emu->mem, &info);
    if (entry == 0) return false;

    emu->entry     = entry;
    emu->rf.pc     = entry;
    emu->state     = EMU_RUNNING;
    emu->exit_code = 0;

    printf("[EMU] pc set to entry = 0x%08X\n", entry);
    printf("[EMU] .text: [0x%08X — 0x%08X]\n", info.text_start, info.text_end);
    if (info.data_start != 0xFFFFFFFF) {
        printf("[EMU] .data: [0x%08X — 0x%08X]\n", info.data_start, info.data_end);
    }
    if (info.bss_start) {
        printf("[EMU] .bss:  [0x%08X — 0x%08X]\n", info.bss_start, info.bss_end);
    }
    return true;
}

u64 emu_run(emulator_t *emu, u64 max_steps)
{
    u64 steps = 0;
    decoded_inst_t inst;

    while (emu->state == EMU_RUNNING) {
        if (max_steps > 0 && steps >= max_steps) break;
        if (!emu_step(emu, &inst)) break;
        steps++;
    }

    return steps;
}

bool emu_step(emulator_t *emu, decoded_inst_t *inst_out)
{
    u32 raw;
    u32 pc = emu->rf.pc;

    /* 1. Fetch: 从内存取出 32-bit 指令 */
    if (!memory_read32(&emu->mem, pc, &raw)) {
        printf("[EMU] fetch failed at pc=0x%08X\n", pc);
        emu->state = EMU_EXCEPTION;
        return false;
    }

    /* 2. Decode */
    if (!decode(raw, inst_out)) {
        printf("[EMU] decode failed at pc=0x%08X raw=0x%08X\n", pc, raw);
        emu->state = EMU_EXCEPTION;
        return false;
    }

    /* 3. Execute (内部更新 pc 和 state) */
    if (!execute(emu, inst_out)) {
        return false;
    }

    /* 4. 周期计数 */
    emu->csr.mcycle++;

    return true;
}

void emu_dump_state(emulator_t *emu)
{
    printf("\n=== Emulator State ===\n");
    printf("  state = %d\n", emu->state);
    printf("  pc    = 0x%08X\n", emu->rf.pc);
    if (emu->state == EMU_HALTED) {
        printf("  exit_code = %u\n", emu->exit_code);
    }
    regfile_dump(&emu->rf);
    csr_dump(&emu->csr);
}
