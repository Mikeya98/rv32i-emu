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
    case OP_LOAD_FP: return FMT_I;
    case OP_STORE:  return FMT_S;
    case OP_STORE_FP: return FMT_S;
    case OP_AMO:    return FMT_R;
    case OP_FP:     return FMT_R;
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
    inst->len    = 4;
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

/* =========================== RV32C 展开 =========================== */

/*
 * 压缩指令不单独实现执行器, 而是展开成等价的 32-bit 指令, 再交给
 * 原来的 decode/execute。好处是 ALU、访存、分支的语义只有一份。
 *
 * 编码器的参数顺序尽量贴近 RISC-V 手册, 便于对照。
 */
static u32 enc_r(u8 f7, u8 rs2, u8 rs1, u8 f3, u8 rd, u8 op)
{
    return ((u32)(f7 & 0x7F) << 25) | ((u32)(rs2 & 0x1F) << 20) |
           ((u32)(rs1 & 0x1F) << 15) | ((u32)(f3 & 7) << 12) |
           ((u32)(rd & 0x1F) << 7) | (op & 0x7F);
}

static u32 enc_i(s32 imm, u8 rs1, u8 f3, u8 rd, u8 op)
{
    return (((u32)imm & 0xFFFu) << 20) | ((u32)(rs1 & 0x1F) << 15) |
           ((u32)(f3 & 7) << 12) | ((u32)(rd & 0x1F) << 7) | (op & 0x7F);
}

static u32 enc_s(s32 imm, u8 rs2, u8 rs1, u8 f3, u8 op)
{
    u32 u = (u32)imm;
    return (((u >> 5) & 0x7F) << 25) | ((u32)(rs2 & 0x1F) << 20) |
           ((u32)(rs1 & 0x1F) << 15) | ((u32)(f3 & 7) << 12) |
           ((u & 0x1F) << 7) | (op & 0x7F);
}

static u32 enc_b(s32 imm, u8 rs2, u8 rs1, u8 f3, u8 op)
{
    u32 u = (u32)imm;
    return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25) |
           ((u32)(rs2 & 0x1F) << 20) | ((u32)(rs1 & 0x1F) << 15) |
           ((u32)(f3 & 7) << 12) | (((u >> 1) & 0xFu) << 8) |
           (((u >> 11) & 1u) << 7) | (op & 0x7F);
}

static u32 enc_u(s32 imm, u8 rd, u8 op)
{
    return ((u32)imm & 0xFFFFF000u) | ((u32)(rd & 0x1F) << 7) | (op & 0x7F);
}

static u32 enc_j(s32 imm, u8 rd, u8 op)
{
    u32 u = (u32)imm;
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
           (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12) |
           ((u32)(rd & 0x1F) << 7) | (op & 0x7F);
}

/* 压缩指令的立即数位域提取 */
#define C_OP(c)     ((c) & 3u)
#define C_F3(c)     (((c) >> 13) & 7u)
#define C_RD(c)     (((c) >> 7) & 0x1Fu)
#define C_RS2(c)    (((c) >> 2) & 0x1Fu)
#define C_RS1P(c)   (8u + (((c) >> 7) & 7u))
#define C_RDP(c)    (8u + (((c) >> 2) & 7u))
#define C_RS2P(c)   (8u + (((c) >> 2) & 7u))

/* CI 型立即数: imm[5]=bit12, imm[4:0]=bits[6:2] */
static u32 c_imm6(u16 c)
{
    return (((c >> 12) & 1u) << 5) | ((c >> 2) & 0x1Fu);
}

static u32 c_uimm_addi4spn(u16 c)
{
    return (((c >> 11) & 3u) << 4) | (((c >> 7) & 0xFu) << 6) |
           (((c >> 6) & 1u) << 2) | (((c >> 5) & 1u) << 3);
}

static u32 c_uimm_lw(u16 c)
{
    return (((c >> 10) & 7u) << 3) | (((c >> 6) & 1u) << 2) |
           (((c >> 5) & 1u) << 6);
}

static u32 c_uimm_lwsp(u16 c)
{
    return (((c >> 12) & 1u) << 5) | (((c >> 4) & 7u) << 2) |
           (((c >> 2) & 3u) << 6);
}

static u32 c_uimm_swsp(u16 c)
{
    return (((c >> 9) & 0xFu) << 2) | (((c >> 7) & 3u) << 6);
}

/* C.BEQZ/C.BNEZ: imm[8]=12, [4:3]=11:10, [7:6]=6:5, [2:1]=4:3, [5]=2 */
static u32 c_imm_b(u16 c)
{
    return (((c >> 12) & 1u) << 8) | (((c >> 10) & 3u) << 3) |
           (((c >> 5) & 3u) << 6) | (((c >> 3) & 3u) << 1) |
           (((c >> 2) & 1u) << 5);
}

/* C.J/C.JAL: imm[11]=12, [4]=11, [9:8]=10:9, [10]=8,
 *            [6]=7, [7]=6, [3:1]=5:3, [5]=2 */
static u32 c_imm_j(u16 c)
{
    return (((c >> 12) & 1u) << 11) | (((c >> 11) & 1u) << 4) |
           (((c >> 9) & 3u) << 8) | (((c >> 8) & 1u) << 10) |
           (((c >> 7) & 1u) << 6) | (((c >> 6) & 1u) << 7) |
           (((c >> 3) & 7u) << 1) | (((c >> 2) & 1u) << 5);
}

/* C.ADDI16SP: imm[9]=12, [4]=6, [6]=5, [8:7]=4:3, [5]=2 */
static u32 c_imm_addi16sp(u16 c)
{
    return (((c >> 12) & 1u) << 9) | (((c >> 6) & 1u) << 4) |
           (((c >> 5) & 1u) << 6) | (((c >> 3) & 3u) << 7) |
           (((c >> 2) & 1u) << 5);
}

bool expand_compressed(u16 c, u32 *out32)
{
    u32 q = C_OP(c);
    u32 f3 = C_F3(c);
    u32 rd = C_RD(c);
    u32 rs2 = C_RS2(c);
    u32 imm;
    s32 simm;

    if (!out32) {
        return false;
    }

    /* ======================= Quadrant 0 ======================= */
    if (q == 0) {
        switch (f3) {
        case 0: /* C.ADDI4SPN: addi rd', sp, nzuimm */
            imm = c_uimm_addi4spn(c);
            if (imm == 0) return false;             /* 保留编码 */
            *out32 = enc_i((s32)imm, 2, 0, C_RDP(c), 0x13);
            return true;

        case 2: /* C.LW: lw rd', uimm(rs1') */
            imm = c_uimm_lw(c);
            *out32 = enc_i((s32)imm, C_RS1P(c), 2, C_RDP(c), 0x03);
            return true;

        case 6: /* C.SW: sw rs2', uimm(rs1') */
            imm = c_uimm_lw(c);
            *out32 = enc_s((s32)imm, C_RS2P(c), C_RS1P(c), 2, 0x23);
            return true;

        default:
            /* C.FLD / C.FLW / C.FSD / C.FSW / reserved: 没有 F/D 扩展 */
            return false;
        }
    }

    /* ======================= Quadrant 1 ======================= */
    if (q == 1) {
        switch (f3) {
        case 0: /* C.NOP / C.ADDI */
            simm = sext(c_imm6(c), 5);
            *out32 = enc_i(simm, rd, 0, rd, 0x13);
            return true;

        case 1: /* RV32: C.JAL → jal x1, offset */
            simm = sext(c_imm_j(c), 11);
            *out32 = enc_j(simm, 1, 0x6F);
            return true;

        case 2: /* C.LI: addi rd, x0, imm */
            simm = sext(c_imm6(c), 5);
            *out32 = enc_i(simm, 0, 0, rd, 0x13);
            return true;

        case 3:
            if (rd == 2) {
                /* C.ADDI16SP: addi sp, sp, imm */
                imm = c_imm_addi16sp(c);
                if (imm == 0) return false;
                simm = sext(imm, 9);
                *out32 = enc_i(simm, 2, 0, 2, 0x13);
                return true;
            }
            if (rd == 0) {
                return false;                       /* 保留编码 */
            }
            /* C.LUI: lui rd, nzimm[17:12] */
            imm = c_imm6(c);
            if (imm == 0) return false;
            simm = sext(imm, 5) << 12;
            *out32 = enc_u(simm, rd, 0x37);
            return true;

        case 4: {
            /* 算术/逻辑: 先看 funct2 */
            u32 f2 = (c >> 10) & 3u;

            if (f2 == 0 || f2 == 1) {
                /* C.SRLI / C.SRAI: shamt[5] 在 RV32 上必须为 0 */
                imm = (((c >> 12) & 1u) << 5) | ((c >> 2) & 0x1Fu);
                if (imm >= 32u) return false;
                /* 注意: 这里只有一个寄存器字段 (bits[9:7]), 既是源也是目标 */
                *out32 = enc_i((s32)(imm | (f2 == 1 ? 0x400u : 0u)),
                               C_RS1P(c), 5, C_RS1P(c), 0x13);
                return true;
            }
            if (f2 == 2) {
                /* C.ANDI */
                simm = sext(c_imm6(c), 5);
                *out32 = enc_i(simm, C_RS1P(c), 7, C_RS1P(c), 0x13);
                return true;
            }

            /* f2 == 3: C.SUB / C.XOR / C.OR / C.AND (bit12=0)
             *          C.SUBW / C.ADDW (bit12=1, RV64 only) */
            if ((c >> 12) & 1u) {
                return false;                       /* RV64 指令 */
            }
            switch ((c >> 5) & 3u) {
            case 0:
                *out32 = enc_r(0x20, C_RS2P(c), C_RS1P(c), 0, C_RS1P(c), 0x33);
                return true;
            case 1:
                *out32 = enc_r(0x00, C_RS2P(c), C_RS1P(c), 4, C_RS1P(c), 0x33);
                return true;
            case 2:
                *out32 = enc_r(0x00, C_RS2P(c), C_RS1P(c), 6, C_RS1P(c), 0x33);
                return true;
            default:
                *out32 = enc_r(0x00, C_RS2P(c), C_RS1P(c), 7, C_RS1P(c), 0x33);
                return true;
            }
        }

        case 5: /* C.J → jal x0, offset */
            simm = sext(c_imm_j(c), 11);
            *out32 = enc_j(simm, 0, 0x6F);
            return true;

        case 6: /* C.BEQZ → beq rs1', x0, offset */
            simm = sext(c_imm_b(c), 8);
            *out32 = enc_b(simm, 0, C_RS1P(c), 0, 0x63);
            return true;

        default: /* C.BNEZ → bne rs1', x0, offset */
            simm = sext(c_imm_b(c), 8);
            *out32 = enc_b(simm, 0, C_RS1P(c), 1, 0x63);
            return true;
        }
    }

    /* ======================= Quadrant 2 ======================= */
    if (q == 2) {
        switch (f3) {
        case 0: /* C.SLLI: slli rd, rd, shamt */
            imm = (((c >> 12) & 1u) << 5) | ((c >> 2) & 0x1Fu);
            if (imm >= 32u) return false;
            *out32 = enc_i((s32)imm, rd, 1, rd, 0x13);
            return true;

        case 2: /* C.LWSP: lw rd, uimm(sp) */
            if (rd == 0) return false;              /* 保留编码 */
            imm = c_uimm_lwsp(c);
            *out32 = enc_i((s32)imm, 2, 2, rd, 0x03);
            return true;

        case 4:
            if (((c >> 12) & 1u) == 0) {
                if (rs2 == 0) {
                    /* C.JR: jalr x0, 0(rs1) */
                    if (rd == 0) return false;      /* 保留编码 */
                    *out32 = enc_i(0, rd, 0, 0, 0x67);
                } else {
                    /* C.MV: add rd, x0, rs2 */
                    *out32 = enc_r(0, rs2, 0, 0, rd, 0x33);
                }
                return true;
            }

            if (rs2 == 0) {
                if (rd == 0) {
                    /* C.EBREAK */
                    *out32 = 0x00100073u;
                } else {
                    /* C.JALR: jalr x1, 0(rs1) */
                    *out32 = enc_i(0, rd, 0, 1, 0x67);
                }
                return true;
            }

            /* C.ADD: add rd, rd, rs2 */
            *out32 = enc_r(0, rs2, rd, 0, rd, 0x33);
            return true;

        case 6: /* C.SWSP: sw rs2, uimm(sp) */
            imm = c_uimm_swsp(c);
            *out32 = enc_s((s32)imm, rs2, 2, 2, 0x23);
            return true;

        default:
            /* C.FLDSP / C.FLWSP / C.FSDSP / C.FSWSP / reserved */
            return false;
        }
    }

    return false;
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

/* RV32A: funct5 在 [31:27] (decoded 里是 funct7 >> 2) */
static const char *amo_name(u8 f5)
{
    switch (f5) {
    case 0x02: return "lr.w";
    case 0x03: return "sc.w";
    case 0x00: return "amoadd.w";
    case 0x01: return "amoswap.w";
    case 0x04: return "amoxor.w";
    case 0x08: return "amoor.w";
    case 0x0C: return "amoand.w";
    case 0x10: return "amomin.w";
    case 0x14: return "amomax.w";
    case 0x18: return "amominu.w";
    case 0x1C: return "amomaxu.w";
    default:   return NULL;
    }
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
        } else if (inst->raw == 0x30200073) {
            snprintf(buf, buf_size, "mret");
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

    case OP_LOAD_FP:
        if (inst->funct3 == 2) {
            snprintf(buf, buf_size, "flw f%u, %d(%s)",
                     inst->rd, inst->imm, rn(inst->rs1));
        } else if (inst->funct3 == 3) {
            snprintf(buf, buf_size, "fld f%u, %d(%s)",
                     inst->rd, inst->imm, rn(inst->rs1));
        }
        break;

    case OP_STORE_FP:
        if (inst->funct3 == 2) {
            snprintf(buf, buf_size, "fsw f%u, %d(%s)",
                     inst->rs2, inst->imm, rn(inst->rs1));
        } else if (inst->funct3 == 3) {
            snprintf(buf, buf_size, "fsd f%u, %d(%s)",
                     inst->rs2, inst->imm, rn(inst->rs1));
        }
        break;

    case OP_FP: {
        u32 f7 = inst->funct7;
        const char *sfx = (f7 & 1u) ? "d" : "s";   /* S/D 编码只差最低位 */
        char name[16];

        if (f7 == 0x20 && inst->rs2 == 1) {        /* FCVT.S.D */
            snprintf(buf, buf_size, "fcvt.s.d f%u, f%u",
                     inst->rd, inst->rs1);
        } else if (f7 == 0x21 && inst->rs2 == 0) { /* FCVT.D.S */
            snprintf(buf, buf_size, "fcvt.d.s f%u, f%u",
                     inst->rd, inst->rs1);
        } else if ((f7 & ~1u) == 0x00 || (f7 & ~1u) == 0x04 ||
                   (f7 & ~1u) == 0x08 || (f7 & ~1u) == 0x0C) {
            const char *op = ((f7 & ~1u) == 0x00) ? "fadd" :
                             ((f7 & ~1u) == 0x04) ? "fsub" :
                             ((f7 & ~1u) == 0x08) ? "fmul" : "fdiv";
            snprintf(name, sizeof(name), "%s.%s", op, sfx);
            snprintf(buf, buf_size, "%s f%u, f%u, f%u",
                     name, inst->rd, inst->rs1, inst->rs2);
        } else if ((f7 & ~1u) == 0x2C) {
            snprintf(name, sizeof(name), "fsqrt.%s", sfx);
            snprintf(buf, buf_size, "%s f%u, f%u",
                     name, inst->rd, inst->rs1);
        } else if ((f7 & ~1u) == 0x10) {
            const char *op = (inst->funct3 == 0) ? "fsgnj" :
                             (inst->funct3 == 1) ? "fsgnjn" :
                             (inst->funct3 == 2) ? "fsgnjx" : NULL;
            if (op) {
                snprintf(name, sizeof(name), "%s.%s", op, sfx);
                snprintf(buf, buf_size, "%s f%u, f%u, f%u",
                         name, inst->rd, inst->rs1, inst->rs2);
            }
        } else if ((f7 & ~1u) == 0x14) {
            const char *op = (inst->funct3 == 0) ? "fmin" :
                             (inst->funct3 == 1) ? "fmax" : NULL;
            if (op) {
                snprintf(name, sizeof(name), "%s.%s", op, sfx);
                snprintf(buf, buf_size, "%s f%u, f%u, f%u",
                         name, inst->rd, inst->rs1, inst->rs2);
            }
        } else if ((f7 & ~1u) == 0x50) {
            const char *op = (inst->funct3 == 0) ? "fle" :
                             (inst->funct3 == 1) ? "flt" :
                             (inst->funct3 == 2) ? "feq" : NULL;
            if (op) {
                snprintf(name, sizeof(name), "%s.%s", op, sfx);
                snprintf(buf, buf_size, "%s %s, f%u, f%u",
                         name, rn(inst->rd), inst->rs1, inst->rs2);
            }
        } else if ((f7 & ~1u) == 0x60) {
            const char *src = (inst->rs2 == 0) ? "w" :
                              (inst->rs2 == 1) ? "wu" : NULL;
            if (src) {
                snprintf(name, sizeof(name), "fcvt.%s.%s", src, sfx);
                snprintf(buf, buf_size, "%s %s, f%u",
                         name, rn(inst->rd), inst->rs1);
            }
        } else if ((f7 & ~1u) == 0x68) {
            const char *src = (inst->rs2 == 0) ? "w" :
                              (inst->rs2 == 1) ? "wu" : NULL;
            if (src) {
                snprintf(name, sizeof(name), "fcvt.%s.%s", sfx, src);
                snprintf(buf, buf_size, "%s f%u, %s",
                         name, inst->rd, rn(inst->rs1));
            }
        } else if ((f7 & ~1u) == 0x70) {
            if (inst->funct3 == 0 && f7 == 0x70) {
                snprintf(buf, buf_size, "fmv.x.w %s, f%u",
                         rn(inst->rd), inst->rs1);
            } else if (inst->funct3 == 1) {
                snprintf(name, sizeof(name), "fclass.%s", sfx);
                snprintf(buf, buf_size, "%s %s, f%u",
                         name, rn(inst->rd), inst->rs1);
            }
        } else if (f7 == 0x78) {
            snprintf(buf, buf_size, "fmv.w.x f%u, %s",
                     inst->rd, rn(inst->rs1));
        }
        break;
    }

    case OP_AMO: {
        const char *n = amo_name(inst->funct7 >> 2);
        if (n && inst->funct3 == 2) {
            if ((inst->funct7 >> 2) == 0x02) {
                snprintf(buf, buf_size, "%s %s, (%s)", n, rn(inst->rd), rn(inst->rs1));
            } else {
                snprintf(buf, buf_size, "%s %s, %s, (%s)", n,
                         rn(inst->rd), rn(inst->rs2), rn(inst->rs1));
            }
        }
        break;
    }

    default:
        break;
    }

    /* 没识别的指令统一打印原始机器码 */
    if (buf[0] == '\0') {
        snprintf(buf, buf_size, ".word 0x%08X", inst->raw);
    }
}
