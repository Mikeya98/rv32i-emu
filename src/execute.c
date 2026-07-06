/**
 * @file    execute.c
 * @brief   RV32I Emulator — 执行引擎实现
 *
 * 覆盖 RV32I Base Integer ISA 全部指令。
 * execute() 负责:
 *   1. 读取源操作数 (rs1, rs2, imm)
 *   2. 执行操作 (ALU / 访存 / 分支 / CSR)
 *   3. 写回结果 (rd / 内存)
 *   4. 更新 PC (顺序 +4 或被分支/跳转覆盖)
 */

#include "execute.h"
#include <stdio.h>

/* =========================== 辅助 =========================== */

/* 符号扩展: bit n 为符号位，扩展到 32-bit */
static u32 sext8(u32 val)  { return (val & 0x80)   ? (val | 0xFFFFFF00) : val; }
static u32 sext16(u32 val) { return (val & 0x8000) ? (val | 0xFFFF0000) : val; }

/* =========================== 核心 =========================== */

bool execute(emulator_t *emu, decoded_inst_t *inst)
{
    u32 pc      = emu->rf.pc;
    u32 next_pc = pc + 4;          /* 默认顺序执行 */
    u32 rs1_val = reg_read(&emu->rf, inst->rs1);
    u32 rs2_val = reg_read(&emu->rf, inst->rs2);
    u32 result  = 0;
    u32 addr = 0;
    u8  b8;
    u16 b16;
    u32 b32;
    bool ok;

    switch (inst->opcode) {

    /* ===================== ALU (R-type) ===================== */
    case OP_ALU:
        switch (inst->funct3) {
        case F3_ADDSUB: /* ADD 或 SUB */
            result = (inst->funct7 == 0x20)
                     ? rs1_val - rs2_val               /* SUB  */
                     : rs1_val + rs2_val;              /* ADD  */
            break;
        case F3_SLL:    /* SLL: shift left logical */
            result = rs1_val << (rs2_val & 0x1F);
            break;
        case F3_SLT:    /* SLT: set less than (signed) */
            result = ((s32)rs1_val < (s32)rs2_val) ? 1 : 0;
            break;
        case F3_SLTU:   /* SLTU: set less than (unsigned) */
            result = (rs1_val < rs2_val) ? 1 : 0;
            break;
        case F3_XOR:
            result = rs1_val ^ rs2_val;
            break;
        case F3_SRLSRA: /* SRL 或 SRA */
            result = (inst->funct7 == 0x20)
                     ? (u32)((s32)rs1_val >> (rs2_val & 0x1F))  /* SRA */
                     : rs1_val >> (rs2_val & 0x1F);             /* SRL */
            break;
        case F3_OR:
            result = rs1_val | rs2_val;
            break;
        case F3_AND:
            result = rs1_val & rs2_val;
            break;
        default:
            goto unknown;
        }
        reg_write(&emu->rf, inst->rd, result);
        break;

    /* ===================== ALU immediate (I-type) ===================== */
    case OP_ALUI:
        switch (inst->funct3) {
        case F3_ADDSUB: /* ADDI */
            result = rs1_val + inst->imm;
            break;
        case F3_SLL:    /* SLLI */
            result = rs1_val << (inst->imm & 0x1F);
            break;
        case F3_SLT:    /* SLTI */
            result = ((s32)rs1_val < (s32)inst->imm) ? 1 : 0;
            break;
        case F3_SLTU:   /* SLTIU */
            result = (rs1_val < (u32)inst->imm) ? 1 : 0;
            break;
        case F3_XOR:    /* XORI */
            result = rs1_val ^ inst->imm;
            break;
        case F3_SRLSRA: /* SRLI 或 SRAI */
            result = (inst->funct7 == 0x20)
                     ? (u32)((s32)rs1_val >> (inst->imm & 0x1F))  /* SRAI */
                     : rs1_val >> (inst->imm & 0x1F);             /* SRLI */
            break;
        case F3_OR:     /* ORI */
            result = rs1_val | inst->imm;
            break;
        case F3_AND:    /* ANDI */
            result = rs1_val & inst->imm;
            break;
        default:
            goto unknown;
        }
        reg_write(&emu->rf, inst->rd, result);
        break;

    /* ===================== LOAD ===================== */
    case OP_LOAD:
        addr = rs1_val + inst->imm;
        switch (inst->funct3) {
        case F3_LB:   /* LB:  byte, sign-extend */
            if (!memory_read8(&emu->mem, addr, &b8))  goto except;
            result = sext8(b8);
            break;
        case F3_LH:   /* LH:  halfword, sign-extend */
            if (!memory_read16(&emu->mem, addr, &b16)) goto except;
            result = sext16(b16);
            break;
        case F3_LW:   /* LW:  word */
            if (!memory_read32(&emu->mem, addr, &b32)) goto except;
            result = b32;
            break;
        case F3_LBU:  /* LBU: byte, zero-extend */
            if (!memory_read8(&emu->mem, addr, &b8))  goto except;
            result = b8;
            break;
        case F3_LHU:  /* LHU: halfword, zero-extend */
            if (!memory_read16(&emu->mem, addr, &b16)) goto except;
            result = b16;
            break;
        default:
            goto unknown;
        }
        reg_write(&emu->rf, inst->rd, result);
        break;

    /* ===================== STORE ===================== */
    case OP_STORE:
        addr = rs1_val + inst->imm;
        switch (inst->funct3) {
        case F3_SB:   /* SB: store byte */
            ok = memory_write8(&emu->mem, addr, (u8)(rs2_val & 0xFF));
            break;
        case F3_SH:   /* SH: store halfword */
            ok = memory_write16(&emu->mem, addr, (u16)(rs2_val & 0xFFFF));
            break;
        case F3_SW:   /* SW: store word */
            ok = memory_write32(&emu->mem, addr, rs2_val);
            break;
        default:
            goto unknown;
        }
        if (!ok) goto except;
        break;

    /* ===================== BRANCH ===================== */
    case OP_BRANCH:
    {
        bool taken = false;
        switch (inst->funct3) {
        case F3_BEQ:  taken = (rs1_val == rs2_val);             break;
        case F3_BNE:  taken = (rs1_val != rs2_val);             break;
        case F3_BLT:  taken = ((s32)rs1_val <  (s32)rs2_val);  break;
        case F3_BGE:  taken = ((s32)rs1_val >= (s32)rs2_val);  break;
        case F3_BLTU: taken = (rs1_val <  rs2_val);             break;
        case F3_BGEU: taken = (rs1_val >= rs2_val);             break;
        default:      goto unknown;
        }
        if (taken) next_pc = pc + inst->imm;  /* 跳转 */
        /* 不跳转则 next_pc 保持 pc+4 */
        break;
    }

    /* ===================== JAL ===================== */
    case OP_JAL:
        reg_write(&emu->rf, inst->rd, pc + 4);   /* 链接 */
        next_pc = pc + inst->imm;                 /* 跳转 */
        break;

    /* ===================== JALR ===================== */
    case OP_JALR:
        result  = pc + 4;
        next_pc = (rs1_val + inst->imm) & ~1u;    /* 最低位清零 */
        reg_write(&emu->rf, inst->rd, result);     /* 链接 */
        break;

    /* ===================== LUI ===================== */
    case OP_LUI:
        reg_write(&emu->rf, inst->rd, inst->imm);
        break;

    /* ===================== AUIPC ===================== */
    case OP_AUIPC:
        reg_write(&emu->rf, inst->rd, pc + inst->imm);
        break;

    /* ===================== SYSTEM ===================== */
    case OP_SYSTEM:
        if (inst->funct3 == F3_ECALL) {
            /* ECALL: a0 (x10) = exit_code, a7 (x17) = syscall number */
            u32 a0 = reg_read(&emu->rf, 10);
            u32 a7 = reg_read(&emu->rf, 17);
            (void)a7;  /* 暂无不同 syscall 的区分 */
            emu->state     = EMU_HALTED;
            emu->exit_code = a0;
            printf("[SYSTEM] ecall: exit_code=%u\n", a0);
        } else if (inst->funct3 == F3_EBREAK) {
            emu->state     = EMU_HALTED;
            emu->exit_code = 0;
            printf("[SYSTEM] ebreak: halted\n");
        } else {
            /* CSR 指令 */
            u32 csr_addr = inst->imm;  /* CSR 地址在 imm 字段 */
            u32 csr_val, wdata;

            ok  = true;
            csr_val = csr_read(&emu->csr, csr_addr, &ok);
            if (!ok) goto except;  /* 未知 CSR */

            switch (inst->funct3) {
            case F3_CSRRW:   /* CSRRW: rd=old_csr, csr=rs1 */
                wdata = rs1_val;
                break;
            case F3_CSRRS:   /* CSRRS: rd=old_csr, csr=old|rs1 */
                wdata = csr_val | rs1_val;
                break;
            case F3_CSRRC:   /* CSRRC: rd=old_csr, csr=old&~rs1 */
                wdata = csr_val & ~rs1_val;
                break;
            case F3_CSRRWI:  /* CSRRWI: rd=old_csr, csr=uimm (inst->rs1 即 zimm[4:0]) */
                wdata = inst->rs1;
                break;
            case F3_CSRRSI:  /* CSRRSI: rd=old_csr, csr=old|uimm */
                wdata = csr_val | inst->rs1;
                break;
            case F3_CSRRCI:  /* CSRRCI: rd=old_csr, csr=old&~uimm */
                wdata = csr_val & ~inst->rs1;
                break;
            default:
                goto unknown;
            }

            csr_write(&emu->csr, csr_addr, wdata, &ok);
            if (!ok) goto except;

            /* 回写旧 CSR 值到 rd（rd=x0 则丢弃） */
            if (inst->rd != 0) {
                reg_write(&emu->rf, inst->rd, csr_val);
            }
        }
        break;

    /* ===================== FENCE ===================== */
    case OP_FENCE:
        /* 模拟器不需要做任何事 */
        break;

    default:
        goto unknown;
    }

    /* 更新 PC */
    emu->rf.pc = next_pc;

    return true;

unknown:
    printf("[EXEC] UNKNOWN instruction: raw=0x%08X opcode=0x%02X f3=%u f7=%u\n",
           inst->raw, inst->opcode, inst->funct3, inst->funct7);
    emu->state = EMU_EXCEPTION;
    return false;

except:
    printf("[EXEC] MEMORY/CSR exception at pc=0x%08X addr=0x%08X\n", pc, addr);
    emu->state = EMU_EXCEPTION;
    return false;
}

/* =========================== 自测 =========================== */

/* 辅助: 把一条指令写入内存指定偏移 */
static void mem_write_inst(memory_t *mem, u32 offset, u32 raw)
{
    memory_write32(mem, offset, raw);
}

#define CHECK(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", name); \
        fail++; \
    } else { \
        printf("  [OK]   %s\n", name); \
        pass++; \
    } \
} while(0)

void execute_selftest(void)
{
    emulator_t emu;
    u32 val;
    int pass = 0, fail = 0;
    u32 base = 0x0000;  /* 测试程序基址 */

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        RV32I Execute Self-Test                           ║\n");
    printf("║        ALU + Branch + Jump + Load/Store                  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ==================== ADDI ==================== */
    printf("── ADDI (add immediate) ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x1, x0, 42   → x1 = 42 */
    mem_write_inst(&emu.mem, base,     0x02A00093);
    /* addi x2, x0, -1   → x2 = -1 (0xFFFFFFFF) */
    mem_write_inst(&emu.mem, base + 4, 0xFFF00113);
    /* addi x3, x1, 100  → x3 = 42 + 100 = 142 */
    mem_write_inst(&emu.mem, base + 8, 0x06408193);
    /* ecall (halt) */
    mem_write_inst(&emu.mem, base + 12, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 4);

    val = reg_read(&emu.rf, 1);  CHECK(val == 42,          "addi x1, x0, 42");
    val = reg_read(&emu.rf, 2);  CHECK(val == 0xFFFFFFFF,  "addi x2, x0, -1");
    val = reg_read(&emu.rf, 3);  CHECK(val == 142,         "addi x3, x1, 100");
    emu_free(&emu);

    /* ==================== ALU R-type ==================== */
    printf("── ALU R-type ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x1, x0, 10   → x1 = 10 */
    mem_write_inst(&emu.mem, base,      0x00A00093);
    /* addi x2, x0, 3    → x2 = 3 */
    mem_write_inst(&emu.mem, base + 4,  0x00300113);
    /* add  x3, x1, x2   → x3 = 13 */
    mem_write_inst(&emu.mem, base + 8,  0x002081B3);
    /* sub  x4, x1, x2   → x4 = 7 */
    mem_write_inst(&emu.mem, base + 12, 0x40208233);
    /* slt  x5, x2, x1   → x5 = 1 (3 < 10 signed) */
    mem_write_inst(&emu.mem, base + 16, 0x001122B3);
    /* sltu x6, x2, x1   → x6 = 1 (3 < 10 unsigned) */
    mem_write_inst(&emu.mem, base + 20, 0x00113333);
    /* xor  x7, x1, x2   → x7 = 10 ^ 3 = 9 */
    mem_write_inst(&emu.mem, base + 24, 0x0020C3B3);
    /* or   x8, x1, x2   → x8 = 10 | 3 = 11 */
    mem_write_inst(&emu.mem, base + 28, 0x0020E433);
    /* and  x9, x1, x2   → x9 = 10 & 3 = 2 */
    mem_write_inst(&emu.mem, base + 32, 0x0020F4B3);
    /* sll  x10, x1, x2  → x10 = 10 << 3 = 80 */
    mem_write_inst(&emu.mem, base + 36, 0x00209533);
    /* srl  x11, x1, x2  → x11 = 10 >> 3 = 1 */
    mem_write_inst(&emu.mem, base + 40, 0x0020D5B3);
    /* ecall */
    mem_write_inst(&emu.mem, base + 44, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 12);

    val = reg_read(&emu.rf, 1);   CHECK(val == 10, "addi x1,10");
    val = reg_read(&emu.rf, 2);   CHECK(val == 3,  "addi x2,3");
    val = reg_read(&emu.rf, 3);   CHECK(val == 13, "add  x3,x1,x2");
    val = reg_read(&emu.rf, 4);   CHECK(val == 7,  "sub  x4,x1,x2");
    val = reg_read(&emu.rf, 5);   CHECK(val == 1,  "slt  x5,x2,x1");
    val = reg_read(&emu.rf, 6);   CHECK(val == 1,  "sltu x6,x2,x1");
    val = reg_read(&emu.rf, 7);   CHECK(val == 9,  "xor  x7,x1,x2");
    val = reg_read(&emu.rf, 8);   CHECK(val == 11, "or   x8,x1,x2");
    val = reg_read(&emu.rf, 9);   CHECK(val == 2,  "and  x9,x1,x2");
    val = reg_read(&emu.rf, 10);  CHECK(val == 80, "sll  x10,x1,x2");
    val = reg_read(&emu.rf, 11);  CHECK(val == 1,  "srl  x11,x1,x2");
    emu_free(&emu);

    /* ==================== Shift variants ==================== */
    printf("── Shift variants ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x1, x0, -8   → x1 = -8 (0xFFFFFFF8) */
    mem_write_inst(&emu.mem, base,      0xFF800093);
    /* srai x2, x1, 2    → x2 = -8 >> 2 = -2 */
    mem_write_inst(&emu.mem, base + 4,  0x4020D113);
    /* srli x3, x1, 1    → x3 = 0xFFFFFFF8 >> 1 = 0x7FFFFFFC */
    mem_write_inst(&emu.mem, base + 8,  0x0010D193);
    /* slli x4, x1, 4    → x4 = -8 << 4 = -128 */
    mem_write_inst(&emu.mem, base + 12, 0x00409213);
    /* ecall */
    mem_write_inst(&emu.mem, base + 16, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 5);

    val = reg_read(&emu.rf, 2);  CHECK((s32)val == -2,         "srai x2, x1, 2");
    val = reg_read(&emu.rf, 3);  CHECK(val == 0x7FFFFFFC,      "srli x3, x1, 1");
    val = reg_read(&emu.rf, 4);  CHECK((s32)val == -128,       "slli x4, x1, 4");
    emu_free(&emu);

    /* ==================== LUI / AUIPC ==================== */
    printf("── LUI / AUIPC ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* lui  x1, 0x12345  → x1 = 0x12345000 */
    mem_write_inst(&emu.mem, base,      0x123450B7);
    /* auipc x2, 0x10    → x2 = pc(0x4) + 0x10000 = 0x10004 */
    mem_write_inst(&emu.mem, base + 4,  0x00010117);
    /* ecall */
    mem_write_inst(&emu.mem, base + 8,  0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 3);

    val = reg_read(&emu.rf, 1);  CHECK(val == 0x12345000, "lui x1, 0x12345");
    val = reg_read(&emu.rf, 2);  CHECK(val == 0x10004,    "auipc x2, 0x10 (pc=4 + 0x10000)");
    emu_free(&emu);

    /* ==================== Branch (taken) ==================== */
    printf("── Branch (taken) ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x1, x0, 5    → x1 = 5 */
    mem_write_inst(&emu.mem, base,      0x00500093);
    /* addi x2, x0, 5    → x2 = 5 */
    mem_write_inst(&emu.mem, base + 4,  0x00500113);
    /* beq  x1, x2, +8   → 跳过下一条 */
    mem_write_inst(&emu.mem, base + 8,  0x00208463);
    /* addi x3, x0, 999  → 应被跳过 */
    mem_write_inst(&emu.mem, base + 12, 0x3E700193);
    /* addi x4, x0, 1    → x4 = 1 (beq 跳转到达这里) */
    mem_write_inst(&emu.mem, base + 16, 0x00100213);
    /* ecall */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    val = reg_read(&emu.rf, 3);  CHECK(val == 0, "x3 = 0 (skipped)");
    val = reg_read(&emu.rf, 4);  CHECK(val == 1, "x4 = 1 (branch taken)");
    emu_free(&emu);

    /* ==================== Branch (not taken) ==================== */
    printf("── Branch (not taken) ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x1, x0, 5 */
    mem_write_inst(&emu.mem, base,      0x00500093);
    /* addi x2, x0, 3 */
    mem_write_inst(&emu.mem, base + 4,  0x00300113);
    /* bne  x1, x2, +8  → not equal? yes! taken */
    mem_write_inst(&emu.mem, base + 8,  0x00209463);
    /* addi x3, x0, 999 → skipped */
    mem_write_inst(&emu.mem, base + 12, 0x3E700193);
    /* addi x4, x0, 2 */
    mem_write_inst(&emu.mem, base + 16, 0x00200213);
    /* ecall */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    val = reg_read(&emu.rf, 3);  CHECK(val == 0, "x3 = 0 (skipped)");
    val = reg_read(&emu.rf, 4);  CHECK(val == 2, "x4 = 2 (bne taken)");
    emu_free(&emu);

    /* ==================== JAL / JALR ==================== */
    printf("── JAL / JALR ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* jal  x1, +12      → x1 = pc+4 = 4, pc = 0+12 = 12 */
    mem_write_inst(&emu.mem, base,      0x00C000EF);
    /* addi x2, x0, 999  → skipped */
    mem_write_inst(&emu.mem, base + 4,  0x3E700113);
    /* addi x2, x0, 999  → skipped */
    mem_write_inst(&emu.mem, base + 8,  0x3E700113);
    /* addi x3, x0, 7    → x3 = 7 */
    mem_write_inst(&emu.mem, base + 12, 0x00700193);
    /* jalr x4, 0(x1)    → x4 = pc+4 = 20, pc = x1+0 = 4 (跳回去) */
    mem_write_inst(&emu.mem, base + 16, 0x00008267);
    /* addi x5, x0, 1   → will never be reached (infinite loop at 4) */
    /* but we'll add ecall at offset 4+8 to halt */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);
    /* At offset 4 (where we jump back to via jalr), put: */
    /* addi x5, x0, 42 → x5 = 42 */
    mem_write_inst(&emu.mem, base + 4,  0x02A00293);
    /* ecall */
    mem_write_inst(&emu.mem, base + 8,  0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    val = reg_read(&emu.rf, 1);  CHECK(val == 4,    "jal: ra = 4");
    val = reg_read(&emu.rf, 4);  CHECK(val == 20,   "jalr: rd = 20");
    val = reg_read(&emu.rf, 5);  CHECK(val == 42,   "executed after jalr return");
    emu_free(&emu);

    /* ==================== LW / SW ==================== */
    printf("── LW / SW (memory) ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x1, x0, 0x100 → x1 = 256 (memory address) */
    mem_write_inst(&emu.mem, base,      0x10000093);
    /* addi x2, x0, 0xDEADBEEF → pseudo: actually need two instrs */
    /* lui  x2, 0xDEADB → x2 = 0xDEADB000 (sign extended? 0xDEADB << 12 */
    /* Actually 0xDEADB is > 0x7FFFF so it becomes negative in imm_u...
       imm_u just does raw & 0xFFFFF000 which preserves the upper bits.
       So lui x2, 0xDEADB → x2 = 0xDEADB000 */

    /* Let me use a simpler constant. 0x12345678 */
    /* lui  x2, 0x12345 → x2 = 0x12345000 */
    mem_write_inst(&emu.mem, base + 4,  0x12345137);
    /* addi x2, x2, 0x678 → x2 = 0x12345678 */
    mem_write_inst(&emu.mem, base + 8,  0x67810113);
    /* sw   x2, 0(x1)   → mem[256] = 0x12345678 */
    mem_write_inst(&emu.mem, base + 12, 0x0020A023);
    /* lw   x3, 0(x1)   → x3 = mem[256] = 0x12345678 */
    mem_write_inst(&emu.mem, base + 16, 0x0000A183);
    /* ecall */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    val = reg_read(&emu.rf, 2);  CHECK(val == 0x12345678, "x2 = 0x12345678");
    val = reg_read(&emu.rf, 3);  CHECK(val == 0x12345678, "lw x3 = mem[256]");
    /* verify memory directly */
    {
        u32 mem_val;
        memory_read32(&emu.mem, 256, &mem_val);
        CHECK(mem_val == 0x12345678, "mem[256] = 0x12345678");
    }
    emu_free(&emu);

    /* ==================== ECALL exit ==================== */
    printf("── ECALL ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    /* addi x10, x0, 42  → a0 = exit_code 42 */
    mem_write_inst(&emu.mem, base, 0x02A00513);
    /* ecall */
    mem_write_inst(&emu.mem, base + 4, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 2);

    CHECK(emu.state == EMU_HALTED, "ecall → EMU_HALTED");
    CHECK(emu.exit_code == 42,     "exit_code = 42");
    emu_free(&emu);

    /* ==================== 结果汇总 ==================== */
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Execute Self-Test: %d passed, %d failed%*s║\n",
           pass, fail, (int)(36 - snprintf(NULL, 0, "%d", pass) - snprintf(NULL, 0, "%d", fail) - 40), "");
    printf("╚══════════════════════════════════════════════════════════╝\n");
}

