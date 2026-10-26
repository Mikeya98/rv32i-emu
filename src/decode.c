/**
 * @file    decode.c
 * @brief   RV32I Emulator — 指令解码实现
 *
 * 将 32-bit 机器码解析为 decoded_inst_t。
 * RISC-V 指令格式 (小端):
 *   R-type: funct7[31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
 *   I-type: imm[31:20] | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
 *   S-type: imm[11:5][31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[4:0][11:7] | opcode[6:0]
 *   B-type: imm[12][31] | imm[10:5][30:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[4:1][11:8] | imm[11][7] | opcode[6:0]
 *   U-type: imm[31:12][31:12] | rd[11:7] | opcode[6:0]
 *   J-type: imm[20][31] | imm[10:1][30:21] | imm[11][20] | imm[19:12][19:12] | rd[11:7] | opcode[6:0]
 */

#include "decode.h"
#include <stdio.h>

/* =========================== 字段提取宏 =========================== */
#define OPCODE(x)   ((x) & 0x7F)
#define RD(x)       (((x) >> 7)  & 0x1F)
#define FUNCT3(x)   (((x) >> 12) & 0x07)
#define RS1(x)      (((x) >> 15) & 0x1F)
#define RS2(x)      (((x) >> 20) & 0x1F)
#define FUNCT7(x)   (((x) >> 25) & 0x7F)

/* 立即数符号扩展 (从 bit N 开始) */
static s32 sext(u32 val, u32 n)
{
    u32 sign = (val >> n) & 1;
    if (sign) {
        u32 mask = ~((1u << (n + 1)) - 1);
        return (s32)(val | mask);
    }
    return (s32)(val & ((1u << (n + 1)) - 1));
}

/* =========================== 立即数提取 =========================== */
static s32 imm_i(u32 raw)
{
    return sext((raw >> 20) & 0xFFF, 11);   /* bits [31:20] */
}

static s32 imm_s(u32 raw)
{
    u32 lo = (raw >> 7)  & 0x1F;            /* bits [11:7]  */
    u32 hi = (raw >> 25) & 0x7F;             /* bits [31:25] */
    return sext((hi << 5) | lo, 11);
}

static s32 imm_b(u32 raw)
{
    u32 b11   = (raw >> 7)  & 0x01;        /* bit  [11]    */
    u32 b4_1  = (raw >> 8)  & 0x0F;        /* bits [11:8]  */
    u32 b10_5 = (raw >> 25) & 0x3F;        /* bits [30:25] */
    u32 b12   = (raw >> 31) & 0x01;        /* bit  [31]    */
    return sext((b12 << 12) | (b11 << 11) | (b10_5 << 5) | (b4_1 << 1), 12);
}

static s32 imm_u(u32 raw)
{
    return (s32)(raw & 0xFFFFF000);          /* bits [31:12], upper 20-bit */
}

static s32 imm_j(u32 raw)
{
    u32 b19_12 = (raw >> 12) & 0xFF;       /* bits [19:12] */
    u32 b11    = (raw >> 20) & 0x01;       /* bit  [11]    */
    u32 b10_1  = (raw >> 21) & 0x3FF;      /* bits [30:21] */
    u32 b20    = (raw >> 31) & 0x01;       /* bit  [31]    */
    return sext((b20 << 20) | (b19_12 << 12) | (b11 << 11) | (b10_1 << 1), 20);
}

/* =========================== 格式判定 =========================== */
static inst_fmt_t get_fmt(opcode_t op)
{
    switch (op) {
    case OP_ALU:    return FMT_R;
    case OP_ALUI:   return FMT_I;
    case OP_LOAD:   return FMT_I;
    case OP_JALR:   return FMT_I;
    case OP_STORE:  return FMT_S;
    case OP_BRANCH: return FMT_B;
    case OP_LUI:    return FMT_U;
    case OP_AUIPC:  return FMT_U;
    case OP_JAL:    return FMT_J;
    case OP_SYSTEM: return FMT_I;  /* ECALL/CSR */
    default:        return FMT_UNKNOWN;
    }
}

/* =========================== 公共 API =========================== */

bool decode(u32 raw, decoded_inst_t *inst)
{
    opcode_t op = (opcode_t)OPCODE(raw);

    inst->raw    = raw;
    inst->opcode = op;
    inst->fmt    = get_fmt(op);

    if (inst->fmt == FMT_UNKNOWN) return false;

    inst->rd     = RD(raw);
    inst->funct3 = FUNCT3(raw);
    inst->rs1    = RS1(raw);
    inst->rs2    = RS2(raw);
    inst->funct7 = FUNCT7(raw);

    switch (inst->fmt) {
    case FMT_R: inst->imm = 0;    break;
    case FMT_I: inst->imm = imm_i(raw); break;
    case FMT_S: inst->imm = imm_s(raw); break;
    case FMT_B: inst->imm = imm_b(raw); break;
    case FMT_U: inst->imm = imm_u(raw); break;
    case FMT_J: inst->imm = imm_j(raw); break;
    default:    inst->imm = 0;    break;
    }

    return true;
}

/* =========================== 反汇编 (调试用) =========================== */

/* ABI 寄存器名, 反汇编和调试器都用这一份 */
static const char *const reg_name[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10","s11","t3", "t4", "t5", "t6"
};

static const char *rn(u32 idx)
{
    return (idx < 32) ? reg_name[idx] : "?";
}

/* R-type ALU 助记符: funct3 + funct7 决定 (含 RV32M) */
static const char *alu_name(u8 f3, u8 f7)
{
    static const char *const base[8] = {
        "add", "sll", "slt", "sltu", "xor", "srl", "or", "and"
    };
    static const char *const alt[8] = {
        "sub", NULL, NULL, NULL, NULL, "sra", NULL, NULL
    };
    static const char *const m[8] = {
        "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"
    };

    if (f3 >= 8) return NULL;
    if (f7 == 0x01) return m[f3];
    if (f7 == 0x20) return alt[f3];
    if (f7 == 0x00) return base[f3];
    return NULL;
}

static const char *alui_name(u8 f3, u8 f7)
{
    static const char *const n[8] = {
        "addi", "slli", "slti", "sltiu", "xori", "srli", "ori", "andi"
    };
    if (f3 >= 8) return NULL;
    if (f3 == 5 && f7 == 0x20) return "srai";
    if (f3 == 1 && f7 != 0x00) return NULL;   /* 当前不区分 RV32I 的 shamt 高位 */
    if (f3 == 5 && f7 != 0x00 && f7 != 0x20) return NULL;
    return n[f3];
}

static const char *load_name(u8 f3)
{
    static const char *const n[8] = { "lb", "lh", "lw", NULL, "lbu", "lhu", NULL, NULL };
    return (f3 < 8) ? n[f3] : NULL;
}

static const char *store_name(u8 f3)
{
    static const char *const n[8] = { "sb", "sh", "sw", NULL, NULL, NULL, NULL, NULL };
    return (f3 < 8) ? n[f3] : NULL;
}

static const char *branch_name(u8 f3)
{
    static const char *const n[8] = {
        "beq", "bne", NULL, NULL, "blt", "bge", "bltu", "bgeu"
    };
    return (f3 < 8) ? n[f3] : NULL;
}

static const char *csr_name(u8 f3)
{
    static const char *const n[8] = {
        NULL, "csrrw", "csrrs", "csrrc", NULL, "csrrwi", "csrrsi", "csrrci"
    };
    return (f3 < 8) ? n[f3] : NULL;
}

void decode_disasm(decoded_inst_t *inst, char *buf, u32 buf_size)
{
    const char *mn;

    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    switch (inst->opcode) {
    case OP_ALU:
        mn = alu_name(inst->funct3, inst->funct7);
        if (mn) {
            snprintf(buf, buf_size, "%s %s, %s, %s",
                     mn, rn(inst->rd), rn(inst->rs1), rn(inst->rs2));
        }
        break;

    case OP_ALUI:
        mn = alui_name(inst->funct3, inst->funct7);
        if (mn) {
            if (inst->funct3 == 1 || inst->funct3 == 5) {
                /* 移位: 第三操作数是 shamt */
                snprintf(buf, buf_size, "%s %s, %s, %u",
                         mn, rn(inst->rd), rn(inst->rs1), inst->imm & 0x1F);
            } else {
                snprintf(buf, buf_size, "%s %s, %s, %d",
                         mn, rn(inst->rd), rn(inst->rs1), inst->imm);
            }
        }
        break;

    case OP_LOAD:
        mn = load_name(inst->funct3);
        if (mn) {
            snprintf(buf, buf_size, "%s %s, %d(%s)",
                     mn, rn(inst->rd), inst->imm, rn(inst->rs1));
        }
        break;

    case OP_STORE:
        mn = store_name(inst->funct3);
        if (mn) {
            snprintf(buf, buf_size, "%s %s, %d(%s)",
                     mn, rn(inst->rs2), inst->imm, rn(inst->rs1));
        }
        break;

    case OP_BRANCH:
        mn = branch_name(inst->funct3);
        if (mn) {
            snprintf(buf, buf_size, "%s %s, %s, %+d",
                     mn, rn(inst->rs1), rn(inst->rs2), inst->imm);
        }
        break;

    case OP_JAL:
        snprintf(buf, buf_size, "jal %s, %+d", rn(inst->rd), inst->imm);
        break;

    case OP_JALR:
        snprintf(buf, buf_size, "jalr %s, %d(%s)",
                 rn(inst->rd), inst->imm, rn(inst->rs1));
        break;

    case OP_LUI:
        snprintf(buf, buf_size, "lui %s, 0x%X",
                 rn(inst->rd), (u32)inst->imm >> 12);
        break;

    case OP_AUIPC:
        snprintf(buf, buf_size, "auipc %s, 0x%X",
                 rn(inst->rd), (u32)inst->imm >> 12);
        break;

    case OP_SYSTEM:
        if (inst->raw == 0x00000073) {
            snprintf(buf, buf_size, "ecall");
        } else if (inst->raw == 0x00100073) {
            snprintf(buf, buf_size, "ebreak");
        } else {
            mn = csr_name(inst->funct3);
            if (mn) {
                if (inst->funct3 >= 5) {
                    snprintf(buf, buf_size, "%s %s, 0x%X, %u",
                             mn, rn(inst->rd), inst->imm, inst->rs1);
                } else {
                    snprintf(buf, buf_size, "%s %s, 0x%X, %s",
                             mn, rn(inst->rd), inst->imm, rn(inst->rs1));
                }
            }
        }
        break;

    case OP_FENCE:
        snprintf(buf, buf_size, "fence");
        break;

    default:
        break;
    }

    /* 没识别的指令统一打印原始机器码 */
    if (buf[0] == '\0') {
        snprintf(buf, buf_size, ".word 0x%08X", inst->raw);
    }
}
