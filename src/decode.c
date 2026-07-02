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
static const char *op_name(opcode_t op)
{
    switch (op) {
    case OP_LUI:    return "lui";
    case OP_AUIPC:  return "auipc";
    case OP_JAL:    return "jal";
    case OP_JALR:   return "jalr";
    case OP_BRANCH: return "branch";
    case OP_LOAD:   return "load";
    case OP_STORE:  return "store";
    case OP_ALUI:   return "alu-i";
    case OP_ALU:    return "alu";
    case OP_SYSTEM: return "system";
    default:        return "???";
    }
}

void decode_disasm(decoded_inst_t *inst, char *buf, u32 buf_size)
{
    snprintf(buf, buf_size,
             "[0x%08X] %-7s rd=x%u rs1=x%u rs2=x%u imm=%d (0x%X) f3=%u f7=%u",
             inst->raw, op_name(inst->opcode),
             inst->rd, inst->rs1, inst->rs2,
             inst->imm, inst->imm,
             inst->funct3, inst->funct7);
}
