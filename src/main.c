/**
 * @file    main.c
 * @brief   RV32I Emulator — 入口
 *
 * 用法:
 *   rvemu <riscv-elf>            直接运行到 ecall/ebreak
 *   rvemu -d <riscv-elf>         进入交互式调试器
 *   rvemu --steps N <riscv-elf>  最多执行 N 条指令
 *   rvemu --harts N <riscv-elf>  模拟 N 个 hart (默认 1)
 *   rvemu --selftest             解码 / 执行 / trap / Sv32 / FPU 自测
 */

#include "emu.h"
#include "decode.h"
#include "execute.h"
#include "fpu.h"
#include "debug.h"
#include "trap.h"
#include "mmu.h"
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
    printf("  --harts N        emulate N harts (default 1, max %d)\n", MAX_HARTS);
    printf("  --mem-model M    sc | weak (default sc)\n");
    printf("  --sb-lag N       store-buffer visibility lag (default 2)\n");
    printf("  --lb-lag N       load-buffer writeback lag (default 0 = off)\n");
    printf("  -h, --help       this message\n");
}

int main(int argc, char *argv[])
{
    emulator_t emu;
    const char *path = NULL;
    bool debug = false;
    bool selftest = false;
    u64 max_steps = 0;
    u32 nharts = 1;
    bool weak_mem = false;
    u32 sb_lag = 2;
    u32 lb_lag = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            max_steps = (u64)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--harts") == 0 && i + 1 < argc) {
            nharts = (u32)strtoul(argv[++i], NULL, 0);
            if (nharts < 1 || nharts > MAX_HARTS) {
                printf("bad hart count %u (1..%u)\n", nharts, MAX_HARTS);
                return 1;
            }
        } else if (strcmp(argv[i], "--mem-model") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "weak") == 0) {
                weak_mem = true;
            } else if (strcmp(m, "sc") == 0) {
                weak_mem = false;
            } else {
                printf("bad mem-model %s (sc|weak)\n", m);
                return 1;
            }
        } else if (strcmp(argv[i], "--sb-lag") == 0 && i + 1 < argc) {
            sb_lag = (u32)strtoul(argv[++i], NULL, 0);
            if (sb_lag > 16) {
                printf("sb-lag must be <= 16\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--lb-lag") == 0 && i + 1 < argc) {
            lb_lag = (u32)strtoul(argv[++i], NULL, 0);
            if (lb_lag > 32) {
                printf("lb-lag must be <= 32\n");
                return 1;
            }
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
    printf("│  RV32I Emulator v0.19                │\n");
    printf("│  RV32IMAFC + D + Sv32 + S/U + SMP    │\n");
    printf("│  + CLINT timer / software IRQ (IPI)  │\n");
    printf("│  + PLIC external interrupts (M+S)    │\n");
    printf("│  + Sstc supervisor timer (STIP)      │\n");
    printf("│  + level devices / complete re-raise │\n");
    printf("│  + store/load buffers, weak memory   │\n");
    printf("└──────────────────────────────────────┘\n\n");

    emu_init(&emu, MEM_SIZE);
    emu_init_harts(&emu, nharts);
    emu.weak_mem = weak_mem;    /* v0.18: 必须在 harts 复位之后设置 */
    emu.sb_lag   = sb_lag;
    emu.lb_lag   = lb_lag;      /* v0.19 */

    if (selftest) {
        decode_selftest();
        rvc_selftest();
        execute_selftest();
        emu_selftest();
        plic_selftest();
        plic_smode_selftest();
        plic_level_selftest();
        trap_selftest();
        trap_smode_selftest();
        trap_stimer_selftest();
        mmu_selftest();
        fpu_selftest();
        membuf_selftest();
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
    printf("  pc    = 0x%08X\n", emu.harts[0].rf.pc);

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
