/**
 * @file    main.c
 * @brief   RV32I Emulator — 入口
 *
 * TODO: 实现完整的 fetch-decode-execute 循环。
 * 当前仅打印框架信息。
 */

#include "emu.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    emulator_t emu;

    printf("┌──────────────────────────────────────┐\n");
    printf("│  RV32I Emulator v0.1-dev             │\n");
    printf("│  RISC-V RV32I Base Integer ISA       │\n");
    printf("└──────────────────────────────────────┘\n\n");

    emu_init(&emu, MEM_SIZE);

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        printf("[SELFTEST] decode module self-test:\n");
        /* TODO: decode self-test */
        printf("  (decode tests pending)\n");
    } else if (argc > 1) {
        printf("[LOAD] %s\n", argv[1]);
        if (!emu_load_elf(&emu, argv[1])) {
            printf("  FAILED: could not load ELF file.\n");
            emu_free(&emu);
            return 1;
        }
        printf("  entry = 0x%08X\n", emu.entry);
        printf("  pc    = 0x%08X\n", emu.rf.pc);
        printf("\n[RUN]\n");
        emu_run(&emu, 0);
        emu_dump_state(&emu);
    } else {
        printf("Usage: rvemu <riscv-elf-file>\n");
        printf("       rvemu --selftest\n");
    }

    emu_free(&emu);
    return 0;
}
