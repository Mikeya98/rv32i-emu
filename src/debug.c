/**
 * @file    debug.c
 * @brief   RV32I Emulator — 交互式调试器实现
 *
 * 没有 ptrace、没有 gdbserver，就是一个读命令行的循环，
 * 每条命令直接操作模拟器状态:
 *
 *   s / c  → 调 emu_step()
 *   b      → 断点表, continue 每步之后检查 PC
 *   r      → regfile_dump()
 *   x      → memory_dump()
 *   d      → 取指 + decode + decode_disasm
 */

#include "debug.h"
#include "decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BREAKPOINTS  32
#define CONTINUE_LIMIT   10000000u   /* 防止死循环把终端卡住 */

static u32 breakpoints[MAX_BREAKPOINTS];
static int bp_count = 0;

/* =========================== 参数解析 =========================== */

/* 从字符串里取下一个整数 (支持 0x 前缀), 成功返回 1 */
static int next_u32(const char **ps, u32 *out)
{
    const char *s = *ps;
    char *end;
    unsigned long v;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return 0;

    v = strtoul(s, &end, 0);
    if (end == s) return 0;

    *out = (u32)v;
    *ps = end;
    return 1;
}

/* =========================== 断点表 =========================== */

static int bp_find(u32 addr)
{
    int i;
    for (i = 0; i < bp_count; i++) {
        if (breakpoints[i] == addr) return i;
    }
    return -1;
}

static void bp_add(u32 addr)
{
    if (bp_find(addr) >= 0) {
        printf("breakpoint already set at 0x%08X\n", addr);
        return;
    }
    if (bp_count >= MAX_BREAKPOINTS) {
        printf("breakpoint table full (%d)\n", MAX_BREAKPOINTS);
        return;
    }
    breakpoints[bp_count++] = addr;
    printf("breakpoint %d set at 0x%08X\n", bp_count - 1, addr);
}

static void bp_del(u32 addr)
{
    int idx = bp_find(addr);
    int i;

    if (idx < 0) {
        printf("no breakpoint at 0x%08X\n", addr);
        return;
    }
    for (i = idx; i + 1 < bp_count; i++) {
        breakpoints[i] = breakpoints[i + 1];
    }
    bp_count--;
    printf("breakpoint at 0x%08X deleted\n", addr);
}

static void bp_list(void)
{
    int i;
    if (bp_count == 0) {
        printf("no breakpoints\n");
        return;
    }
    for (i = 0; i < bp_count; i++) {
        printf("  [%d] 0x%08X\n", i, breakpoints[i]);
    }
}

/* =========================== 单条指令显示 =========================== */

static void show_inst(emulator_t *emu, u32 addr)
{
    u32 raw;
    decoded_inst_t inst;
    char buf[128];

    if (!memory_read32(&emu->mem, addr, &raw)) {
        printf("  0x%08X: <unmapped>\n", addr);
        return;
    }
    if (!decode(raw, &inst)) {
        printf("  0x%08X: .word 0x%08X\n", addr, raw);
        return;
    }

    decode_disasm(&inst, buf, sizeof(buf));
    printf("  0x%08X: %s\n", addr, buf);
}

static const char *state_name(emu_state_t s)
{
    switch (s) {
    case EMU_RUNNING:   return "RUNNING";
    case EMU_HALTED:    return "HALTED";
    case EMU_EXCEPTION: return "EXCEPTION";
    default:            return "?";
    }
}

static void show_status(emulator_t *emu)
{
    printf("[%s] pc=0x%08X priv=%s", state_name(emu->state), emu->rf.pc,
           (emu->priv == PRIV_M) ? "M" : "U");
    if (emu->csr.satp & SATP_MODE_BIT) {
        printf(" satp=Sv32@0x%X", (emu->csr.satp & SATP_PPN_MASK) << 12);
    }
    if (emu->state == EMU_HALTED) {
        printf("  exit_code=%u", emu->exit_code);
    }
    printf("\n");
}

/* =========================== 命令实现 =========================== */

static void cmd_step(emulator_t *emu, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        u32 pc;
        decoded_inst_t inst;

        if (emu->state != EMU_RUNNING) {
            printf("emulator is not running\n");
            show_status(emu);
            return;
        }

        pc = emu->rf.pc;
        printf("[step] pc=0x%08X\n", pc);
        show_inst(emu, pc);

        if (!emu_step(emu, &inst)) {
            break;
        }
    }

    if (emu->state == EMU_RUNNING) {
        printf("[next]\n");
        show_inst(emu, emu->rf.pc);
    } else {
        show_status(emu);
    }
}

static void cmd_continue(emulator_t *emu)
{
    u32 steps = 0;
    decoded_inst_t inst;

    while (emu->state == EMU_RUNNING) {
        if (steps >= CONTINUE_LIMIT) {
            printf("stopped after %u steps (limit). Use 's' to continue.\n",
                   CONTINUE_LIMIT);
            return;
        }

        if (!emu_step(emu, &inst)) {
            show_status(emu);
            return;
        }
        steps++;

        if (emu->state != EMU_RUNNING) {
            show_status(emu);
            return;
        }

        /* 每执行完一条再检查, 避免停在当前断点上反复触发 */
        if (bp_find(emu->rf.pc) >= 0) {
            printf("[break] hit breakpoint\n");
            show_status(emu);
            show_inst(emu, emu->rf.pc);
            printf("executed %u instructions\n", steps);
            return;
        }
    }

    show_status(emu);
}

static void cmd_disasm(emulator_t *emu, u32 addr, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        u32 raw;
        decoded_inst_t inst;
        char buf[128];

        if (!memory_read32(&emu->mem, addr, &raw)) {
            printf("  0x%08X: <unmapped>\n", addr);
            return;
        }
        if (!decode(raw, &inst)) {
            printf("  0x%08X: .word 0x%08X\n", addr, raw);
        } else {
            decode_disasm(&inst, buf, sizeof(buf));
            printf("  0x%08X: %s\n", addr, buf);
        }
        addr += 4;
    }
}

static void cmd_regs(emulator_t *emu)
{
    regfile_dump(&emu->rf);
    csr_dump(&emu->csr);
}

static void print_help(void)
{
    printf(
        "commands:\n"
        "  s [n]         step n instructions (default 1)\n"
        "  c             continue until breakpoint / halt\n"
        "  r             dump registers and CSRs\n"
        "  x <addr> [n]  dump n bytes of memory (default 64)\n"
        "  d [addr] [n]  disassemble n instructions (default: pc, 8)\n"
        "  b <addr>      set breakpoint\n"
        "  bl            list breakpoints\n"
        "  bd <addr>     delete breakpoint\n"
        "  bc            clear all breakpoints\n"
        "  h             help\n"
        "  q             quit\n");
}

/* =========================== 主循环 =========================== */

void debug_repl(emulator_t *emu)
{
    char line[128];

    printf("\n=== RV32I Debugger ===\n");
    printf("Type 'h' for help, 'q' to quit.\n");
    show_status(emu);
    if (emu->state == EMU_RUNNING) {
        show_inst(emu, emu->rf.pc);
    }

    while (1) {
        char cmd[16];
        const char *args;
        u32 addr, n;

        printf("(rvdbg) ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        /* 去掉行尾换行 */
        {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
        }

        if (line[0] == '\0') {
            continue;
        }
        if (sscanf(line, "%15s", cmd) != 1) {
            continue;
        }

        args = line + strlen(cmd);
        while (*args == ' ' || *args == '\t') args++;

        if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0) {
            n = 1;
            next_u32(&args, &n);
            cmd_step(emu, n);
        } else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
            cmd_continue(emu);
        } else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "regs") == 0) {
            cmd_regs(emu);
        } else if (strcmp(cmd, "x") == 0) {
            if (!next_u32(&args, &addr)) {
                printf("usage: x <addr> [len]\n");
                continue;
            }
            n = 64;
            next_u32(&args, &n);
            memory_dump(&emu->mem, addr, n);
        } else if (strcmp(cmd, "d") == 0 || strcmp(cmd, "dis") == 0) {
            addr = emu->rf.pc;
            n = 8;
            next_u32(&args, &addr);
            next_u32(&args, &n);
            if (n == 0) n = 8;
            cmd_disasm(emu, addr, n);
        } else if (strcmp(cmd, "b") == 0) {
            if (!next_u32(&args, &addr)) {
                printf("usage: b <addr>\n");
                continue;
            }
            bp_add(addr);
        } else if (strcmp(cmd, "bl") == 0) {
            bp_list();
        } else if (strcmp(cmd, "bd") == 0) {
            if (!next_u32(&args, &addr)) {
                printf("usage: bd <addr>\n");
                continue;
            }
            bp_del(addr);
        } else if (strcmp(cmd, "bc") == 0) {
            bp_count = 0;
            printf("all breakpoints cleared\n");
        } else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0 ||
                   strcmp(cmd, "?") == 0) {
            print_help();
        } else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else {
            printf("unknown command: %s (try 'h')\n", cmd);
        }
    }
}
