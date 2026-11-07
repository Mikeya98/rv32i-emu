/**
 * @file    emu.c
 * @brief   RV32I Emulator — 顶层模拟器实现
 *
 * fetch-decode-execute 主循环。
 */

#include "emu.h"
#include "elf.h"
#include "execute.h"
#include "trap.h"
#include "mmu.h"
#include <stdio.h>
#include <string.h>

void emu_init(emulator_t *emu, u32 mem_size)
{
    (void)mem_size;  /* 当前使用 memory.h 中的固定 MEM_SIZE */

    memory_init(&emu->mem);
    regfile_init(&emu->rf);
    csr_init(&emu->csr);
    trap_init(emu);

    emu->priv = PRIV_M;         /* 复位进 M 态 */

    emu->reservation_valid = 0; /* RV32A: 复位后没有 LR 保留 */
    emu->reservation_addr  = 0;

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
    bool compressed = false;
    u32 pc = emu->rf.pc;

    /*
     * 0. 指令边界上检查定时器中断。
     *
     * 中断是异步的, 只能在两条指令之间进来。trap_check_interrupt
     * 顺便把 mtime 往前推一格。
     */
    if (trap_check_interrupt(emu, pc)) {
        emu->csr.mcycle++;
        return true;            /* 这一"步"没有执行指令, 但跳进了 handler */
    }

    /* 1. Fetch: 取出 16-bit 或 32-bit 指令 (RV32C 允许 2 字节对齐) */
    if ((pc & 1u) != 0) {
        if (trap_raise(emu, EXC_INST_MISALIGNED, pc, pc)) {
            emu->csr.mcycle++;
            return true;
        }
        printf("[EMU] misaligned pc=0x%08X\n", pc);
        emu->state = EMU_EXCEPTION;
        return false;
    }

    /*
     * v0.4: 取指也要过页表。
     * U 态执行到一页没有 X 权限的内存, 或者干脆没映射的地址, 就在这里
     * 报 12 (instruction page fault), mtval = 出错的 PC。
     */
    {
        u32 fetch_pa = 0;
        u16 lo = 0;
        u16 hi = 0;
        bool fetch_ok = true;
        int frc = mmu_translate(emu, pc, ACC_FETCH, &fetch_pa);

        if (frc != 0) {
            if (trap_raise(emu, (u32)frc, pc, pc)) {
                emu->csr.mcycle++;
                return true;
            }
            printf("[EMU] fetch page fault at pc=0x%08X\n", pc);
            emu->state = EMU_EXCEPTION;
            return false;
        }

        /*
         * 先读 16 位。低两位不是 11 就是压缩指令 (RV32C),
         * 否则后面还跟着半个字。
         */
        if (!memory_read16(&emu->mem, fetch_pa, &lo)) {
            if (trap_raise(emu, EXC_INST_ACCESS, pc, pc)) {
                emu->csr.mcycle++;
                return true;
            }
            printf("[EMU] fetch failed at pc=0x%08X\n", pc);
            emu->state = EMU_EXCEPTION;
            return false;
        }

        if ((lo & 3u) != 3u) {
            if (!expand_compressed(lo, &raw)) {
                if (trap_raise(emu, EXC_ILLEGAL_INST, lo, pc)) {
                    emu->csr.mcycle++;
                    return true;
                }
                printf("[EMU] bad compressed inst 0x%04X at pc=0x%08X\n", lo, pc);
                emu->state = EMU_EXCEPTION;
                return false;
            }
            compressed = true;
        } else {
            /*
             * 32-bit 指令。正常情况下在同一个物理页里直接读 32 位;
             * 如果正好压在页尾 (只有 2 字节在本页), 把第二个半字
             * 单独翻译一次, 避免跨页时读到相邻物理页的内容。
             */
            if ((fetch_pa & 0xFFFu) <= 0xFFCu) {
                fetch_ok = memory_read32(&emu->mem, fetch_pa, &raw);
            } else {
                u32 pa2 = 0;
                if (mmu_translate(emu, pc + 2u, ACC_FETCH, &pa2) != 0 ||
                    !memory_read16(&emu->mem, pa2, &hi)) {
                    fetch_ok = false;
                } else {
                    raw = (u32)lo | ((u32)hi << 16);
                }
            }

            if (!fetch_ok) {
                if (trap_raise(emu, EXC_INST_ACCESS, pc, pc)) {
                    emu->csr.mcycle++;
                    return true;
                }
                printf("[EMU] fetch failed at pc=0x%08X\n", pc);
                emu->state = EMU_EXCEPTION;
                return false;
            }
        }
    }

    /* 2. Decode */
    if (!decode(raw, inst_out)) {
        if (trap_raise(emu, EXC_ILLEGAL_INST, raw, pc)) {
            emu->csr.mcycle++;
            return true;
        }
        printf("[EMU] decode failed at pc=0x%08X raw=0x%08X\n", pc, raw);
        emu->state = EMU_EXCEPTION;
        return false;
    }

    /* decode() 默认 len=4, 压缩指令在这里改回 2 (影响 PC 增量和链接值) */
    if (compressed) {
        inst_out->len = 2;
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
    regfile_dump_f(&emu->rf);
    csr_dump(&emu->csr);
}
