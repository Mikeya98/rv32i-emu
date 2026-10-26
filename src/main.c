/**
 * @file    main.c
 * @brief   RV32I Emulator — 入口
 *
 * 用法:
 *   rvemu <riscv-elf>            直接运行到 ecall/ebreak
 *   rvemu -d <riscv-elf>         进入交互式调试器
 *   rvemu --steps N <riscv-elf>  最多执行 N 条指令
 *   rvemu --selftest             解码 / 执行自测
 */

#include "emu.h"
#include "decode.h"
#include "execute.h"
#include "debug.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *prog)
{
    printf("Usage: %s [options] <riscv-elf>\n", prog);
    printf("       %s --selftest\n", prog);
    printf("\nOptions:\n");
    printf("  -d, --debug      interactive debugger\n");
    printf("  --steps N        stop after N instructions\n");
    printf("  -h, --help       this message\n");
}

int main(int argc, char *argv[])
{
    emulator_t emu;
    const char *path = NULL;
    bool debug = false;
    bool selftest = false;
    u64 max_steps = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            max_steps = (u64)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            printf("unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            path = argv[i];
        }
    }

    printf("┌──────────────────────────────────────┐\n");
    printf("│  RV32I Emulator v0.2                 │\n");
    printf("│  RV32I + RV32M + syscall + debugger  │\n");
    printf("└──────────────────────────────────────┘\n\n");

    emu_init(&emu, MEM_SIZE);

    if (selftest) {
        decode_selftest();
        execute_selftest();
        emu_free(&emu);
        return 0;
    }

    if (!path) {
        usage(argv[0]);
        emu_free(&emu);
        return 1;
    }

    printf("[LOAD] %s\n", path);
    if (!emu_load_elf(&emu, path)) {
        printf("  FAILED: could not load ELF file.\n");
        emu_free(&emu);
        return 1;
    }
    printf("  entry = 0x%08X\n", emu.entry);
    printf("  pc    = 0x%08X\n", emu.rf.pc);

    if (debug) {
        debug_repl(&emu);
    } else {
        u64 steps;

        printf("\n[RUN]\n");
        steps = emu_run(&emu, max_steps);
        printf("[RUN] %llu instructions executed\n", (unsigned long long)steps);
        emu_dump_state(&emu);
    }

    emu_free(&emu);
    return 0;
}
