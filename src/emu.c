/**
 * @file    emu.c
 * @brief   RV32I Emulator — 顶层模拟器实现
 *
 * TODO: fetch-decode-execute 主循环。
 */

#include "emu.h"
#include "elf.h"
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
    (void)emu;
    (void)max_steps;
    /* TODO: main loop */
    return 0;
}

bool emu_step(emulator_t *emu, decoded_inst_t *inst_out)
{
    (void)emu;
    (void)inst_out;
    /* TODO: single step */
    return false;
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
