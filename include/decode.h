/**
 * @file    decode.h
 * @brief   RV32I Emulator — 指令解码
 *
 * 将 32-bit 指令字解析为操作码 + 操作数。
 * RV32I 基础整数指令集 (~40 条)。
 */

#ifndef _RVEMU_DECODE_H
#define _RVEMU_DECODE_H

#include "types.h"

/* =========================== 指令格式 =========================== */
typedef enum {
    FMT_R,       /* R-type:  add rd, rs1, rs2             */
    FMT_I,       /* I-type:  addi rd, rs1, imm            */
    FMT_S,       /* S-type:  sw rs2, imm(rs1)             */
    FMT_B,       /* B-type:  beq rs1, rs2, offset         */
    FMT_U,       /* U-type:  lui rd, imm                  */
    FMT_J,       /* J-type:  jal rd, offset               */
    FMT_UNKNOWN,
} inst_fmt_t;

/* =========================== 操作码 =========================== */
typedef enum {
    OP_LUI      = 0x37,   /* 0110111 */
    OP_AUIPC    = 0x17,   /* 0010111 */
    OP_JAL      = 0x6F,   /* 1101111 */
    OP_JALR     = 0x67,   /* 1100111 */
    OP_BRANCH   = 0x63,   /* 1100011 */
    OP_LOAD     = 0x03,   /* 0000011 */
    OP_STORE    = 0x23,   /* 0100011 */
    OP_ALUI     = 0x13,   /* 0010011 */
    OP_ALU      = 0x33,   /* 0110011 */
    OP_FENCE    = 0x0F,   /* 0001111 */
    OP_SYSTEM   = 0x73,   /* 1110011 */
} opcode_t;

/* =========================== funct3 分类 =========================== */
typedef enum {
    F3_ADDSUB   = 0,   F3_SLL     = 1,   F3_SLT     = 2,
    F3_SLTU     = 3,   F3_XOR     = 4,   F3_SRLSRA  = 5,
    F3_OR       = 6,   F3_AND     = 7,
    F3_BEQ      = 0,   F3_BNE     = 1,   F3_BLT     = 4,
    F3_BGE      = 5,   F3_BLTU    = 6,   F3_BGEU    = 7,
    F3_LB       = 0,   F3_LH      = 1,   F3_LW      = 2,
    F3_LBU      = 4,   F3_LHU     = 5,
    F3_SB       = 0,   F3_SH      = 1,   F3_SW      = 2,
    F3_PRIV     = 0,   F3_CSRRW   = 1,   F3_CSRRS   = 2,
    F3_CSRRC    = 3,   F3_CSRRWI  = 5,   F3_CSRRSI  = 6,
    F3_CSRRCI   = 7,
    F3_ECALL    = 0,   F3_EBREAK  = 0,
    F3_MRET     = 0,
} funct3_t;

/* =========================== 解码后的指令 =========================== */
typedef struct {
    u32         raw;       /* 原始 32-bit 指令 */
    inst_fmt_t  fmt;       /* 指令格式 */
    opcode_t    opcode;    /* 操作码 (7-bit) */
    u8          rd;        /* 目标寄存器 (5-bit) */
    u8          rs1;       /* 源寄存器 1 (5-bit) */
    u8          rs2;       /* 源寄存器 2 (5-bit) */
    u8          funct3;    /* 功能码 (3-bit) */
    u8          funct7;    /* 功能码 (7-bit), R-type */
    u32         imm;       /* 立即数 (符号扩展后) */
} decoded_inst_t;

/* --- API --- */

/**
 * @brief  解码一条 32-bit RV32I 指令
 * @param  raw   从内存取出的 32-bit 指令 (小端)
 * @param  inst  输出: 解码后的结构体
 * @return true=成功, false=未知操作码
 */
bool decode(u32 raw, decoded_inst_t *inst);

/* 调试: 打印指令的反汇编 */
void decode_disasm(decoded_inst_t *inst, char *buf, u32 buf_size);

#endif /* _RVEMU_DECODE_H */
