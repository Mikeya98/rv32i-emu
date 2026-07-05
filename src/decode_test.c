/**
 * @file    decode_test.c
 * @brief   RV32I Emulator — 指令解码自测
 *
 * 手写已知机器码，调用 decode() 验证。
 * 覆盖 RV32I 全部 6 种指令格式 (R/I/S/B/U/J)。
 *
 * 注意: 解码器对所有格式统一从 R-type 位布局提取字段。
 * 对于非 R-type 格式，某些"寄存器"位域实际存放立即数片段
 * (如 I-type 的 bits[24:20]=rs2 位置 = imm[9:5])。
 * 这是设计如此——execute 阶段按需选用 inst.imm 或 inst.rs2。
 */

#include "decode.h"
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

/*
 * 解码器对每个 raw 指令从 6 个固定位置提取:
 *   opcode  = raw[ 6: 0]    (7 bits)
 *   rd      = raw[11: 7]    (5 bits)
 *   funct3  = raw[14:12]    (3 bits)
 *   rs1     = raw[19:15]    (5 bits)
 *   rs2     = raw[24:20]    (5 bits)
 *   funct7  = raw[31:25]    (7 bits)
 *
 * 对于不同格式，这些字段的"语义"不同:
 *   R-type: 全部有意义 (rs1/rs2/rd 都是寄存器)
 *   I-type: rs2 位置 = imm[9:5], funct7 位置 = imm[11:5]
 *   S-type: rd  位置 = imm[4:0], funct7 位置 = imm[11:5]
 *   B-type: rd  位置 = imm[4:1|11]
 *   U-type: rs1/rs2/funct3 位置 = imm 的高位
 *   J-type: rs1/rs2/funct3 位置 = imm 的分散位
 *
 * 测试中所有预期值都是根据 raw 指令的 hex 值精确计算,
 * 不假设"无意义字段为 0"。
 */

/* --- 辅助: 从 raw 提取字段 --- */
#define _OP(raw)   ((raw) & 0x7F)
#define _RD(raw)   (((raw) >> 7)  & 0x1F)
#define _F3(raw)   (((raw) >> 12) & 0x07)
#define _RS1(raw)  (((raw) >> 15) & 0x1F)
#define _RS2(raw)  (((raw) >> 20) & 0x1F)
#define _F7(raw)   (((raw) >> 25) & 0x7F)

#define TEST_FULL(name, raw, exp_fmt, exp_op, exp_imm) \
    do {                                                                    \
        decoded_inst_t inst;                                                \
        char disasm[128];                                                   \
        bool ok = decode(raw, &inst);                                       \
        decode_disasm(&inst, disasm, sizeof(disasm));                       \
        if (!ok) {                                                          \
            printf("  [FAIL] %s: decode returned false\n", name);           \
            failed++;                                                       \
        } else if (inst.fmt != exp_fmt                                      \
                || inst.opcode != (exp_op)                                  \
                || inst.rd     != _RD(raw)                                  \
                || inst.rs1    != _RS1(raw)                                 \
                || inst.rs2    != _RS2(raw)                                 \
                || inst.funct3 != _F3(raw)                                  \
                || inst.funct7 != _F7(raw)                                  \
                || inst.imm    != (u32)(s32)(exp_imm)) {                    \
            printf("  [FAIL] %s\n", name);                                  \
            printf("         raw=0x%08X\n", raw);                           \
            printf("         expected: fmt=%d op=0x%02X rd=x%u rs1=x%u"    \
                   " rs2=x%u f3=%u f7=%u imm=%d\n",                        \
                   exp_fmt, (u32)(exp_op), _RD(raw), _RS1(raw),            \
                   _RS2(raw), _F3(raw), _F7(raw), (s32)(exp_imm));        \
            printf("         got:      fmt=%d op=0x%02X rd=x%u rs1=x%u"    \
                   " rs2=x%u f3=%u f7=%u imm=%d\n",                        \
                   inst.fmt, inst.opcode, inst.rd, inst.rs1, inst.rs2,     \
                   inst.funct3, inst.funct7, (s32)inst.imm);                \
            failed++;                                                       \
        } else {                                                            \
            printf("  [OK]   %-30s  %s\n", name, disasm);                  \
            passed++;                                                       \
        }                                                                   \
    } while(0)

/* ================================================================
 * 自测入口
 * ================================================================ */
void decode_selftest(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        RV32I Decode Self-Test                            ║\n");
    printf("║        6 formats x representative instructions           ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");

    /* ==================== R-type ==================== */
    printf("── R-type (register-register) ──\n");
    printf("   Format: funct7[31:25] | rs2[24:20] | rs1[19:15]"
           " | funct3[14:12] | rd[11:7] | opcode[6:0]\n");
    printf("   All 5 fields meaningful: rd, rs1, rs2, funct3, funct7\n\n");

    /* add x5, x10, x11 → f7=0 f3=0 rd=5 rs1=10 rs2=11 op=0x33 */
    /* raw = 0x00B582B3: f7=0 rs2=11 rs1=10 f3=0 rd=5 op=0x33 */
    TEST_FULL("add  x5, x10, x11",  0x00B582B3,  FMT_R, OP_ALU,   0);

    /* sub x3, x4, x5  → f7=0x20 f3=0 rd=3 rs1=4 rs2=5 */
    TEST_FULL("sub  x3, x4, x5",    0x405201B3,  FMT_R, OP_ALU,   0);

    /* sll x2, x3, x4  → f7=0 f3=1 rd=2 rs1=3 rs2=4 */
    TEST_FULL("sll  x2, x3, x4",    0x004191B3,  FMT_R, OP_ALU,   0);

    /* slt x1, x2, x3  → f7=0 f3=2 rd=1 rs1=2 rs2=3 */
    TEST_FULL("slt  x1, x2, x3",    0x003120B3,  FMT_R, OP_ALU,   0);

    /* xor x7, x8, x9  → f7=0 f3=4 rd=7 rs1=8 rs2=9 */
    TEST_FULL("xor  x7, x8, x9",    0x009443B3,  FMT_R, OP_ALU,   0);

    /* srl x6, x7, x8  → f7=0 f3=5 rd=6 rs1=7 rs2=8 */
    TEST_FULL("srl  x6, x7, x8",    0x0083D333,  FMT_R, OP_ALU,   0);

    /* or  x9, x10, x11 → f7=0 f3=6 rd=9 rs1=10 rs2=11 */
    TEST_FULL("or   x9, x10, x11",  0x00B564B3,  FMT_R, OP_ALU,   0);

    /* and x4, x5, x6  → f7=0 f3=7 rd=4 rs1=5 rs2=6 */
    TEST_FULL("and  x4, x5, x6",    0x0062F233,  FMT_R, OP_ALU,   0);

    /* sra x3, x4, x5  → f7=0x20 f3=5 rd=3 rs1=4 rs2=5 */
    TEST_FULL("sra  x3, x4, x5",    0x405251B3,  FMT_R, OP_ALU,   0);

    /* ==================== I-type ==================== */
    printf("\n── I-type (immediate) ──\n");
    printf("   Format: imm[31:20] | rs1[19:15] | funct3[14:12]"
           " | rd[11:7] | opcode[6:0]\n");
    printf("   Note: rs2 位置 = imm[9:5], funct7 位置 = imm[11:5]"
           " (但 exec 用 inst.imm, 忽略 inst.rs2/inst.funct7)\n\n");

    /* addi x1, x0, 0 → f3=0 rd=1 rs1=0 imm=0 */
    TEST_FULL("addi x1, x0, 0",     0x00000093,  FMT_I, OP_ALUI,  0);

    /* addi x2, x3, 42 → f3=0 rd=2 rs1=3 imm=42 */
    /* raw=0x02A18113: imm[11:0]=42=0x02A, rs2=imm[9:5]=0x02A>>5=1, f7=imm[11:5]=0x02A>>5=1 */
    TEST_FULL("addi x2, x3, 42",    0x02A18113,  FMT_I, OP_ALUI,  42);

    /* addi x5, x5, -1 → imm=0xFFF=-1, rs2=0xFFF>>5=0x1F=31, f7=0x1F=31 */
    TEST_FULL("addi x5, x5, -1",    0xFFF28293,  FMT_I, OP_ALUI,  -1);

    /* slti x1, x2, 100 → imm=100=0x064, rs2=0x064>>5=3, f7=3 */
    TEST_FULL("slti x1, x2, 100",   0x06412093,  FMT_I, OP_ALUI,  100);

    /* xori x3, x4, 255 → imm=255=0x0FF, rs2=0xFF>>5=7, f7=7 */
    TEST_FULL("xori x3, x4, 255",   0x0FF24193,  FMT_I, OP_ALUI,  255);

    /* ori  x6, x7, 0 → imm=0 */
    TEST_FULL("ori  x6, x7, 0",     0x0003E313,  FMT_I, OP_ALUI,  0);

    /* andi x8, x9, 0x7FF → imm=2047=0x7FF, rs2=0x7FF>>5=0x3F=63, f7=63 */
    TEST_FULL("andi x8, x9, 2047",  0x7FF4F413,  FMT_I, OP_ALUI,  2047);

    /* slli x10, x11, 16 → imm=16, rs2=16>>5=0 */
    TEST_FULL("slli x10, x11, 16",  0x01059513,  FMT_I, OP_ALUI,  16);

    /* srli x4, x5, 8 → imm=8, rs2=0 */
    TEST_FULL("srli x4, x5, 8",     0x0082D213,  FMT_I, OP_ALUI,  8);

    /* srai x6, x7, 31 → imm=0x41F (f7=0x20, shamt=31), rs2=0x41F>>5=0x20=32 */
    TEST_FULL("srai x6, x7, 31",    0x41F3D313,  FMT_I, OP_ALUI,  0x41F);

    /* ==================== I-type (LOAD) ==================== */
    printf("\n── I-type (load) ──\n\n");

    /* lw  x1, 0(x2) → imm=0, f3=2 */
    TEST_FULL("lw   x1, 0(x2)",     0x00012083,  FMT_I, OP_LOAD,  0);

    /* lw  x3, 8(x4) → imm=8, rs2=0 */
    TEST_FULL("lw   x3, 8(x4)",     0x00822183,  FMT_I, OP_LOAD,  8);

    /* lh  x5, -4(x6) → imm=-4=0xFFC, rs2=0xFFC>>5=0x7F=127 */
    TEST_FULL("lh   x5, -4(x6)",    0xFFC31283,  FMT_I, OP_LOAD,  -4);

    /* lb  x7, 1(x8) → imm=1, rs2=0 */
    TEST_FULL("lb   x7, 1(x8)",     0x00140383,  FMT_I, OP_LOAD,  1);

    /* lbu x9, 255(x10) → imm=255=0x0FF, rs2=7 */
    TEST_FULL("lbu  x9, 255(x10)",  0x0FF54483,  FMT_I, OP_LOAD,  255);

    /* ==================== S-type ==================== */
    printf("\n── S-type (store) ──\n");
    printf("   Format: imm[11:5][31:25] | rs2[24:20] | rs1[19:15]"
           " | funct3[14:12] | imm[4:0][11:7] | opcode[6:0]\n");
    printf("   Note: rd 位置 = imm[4:0], funct7 位置 = imm[11:5]"
           " (exec 用 inst.imm, 忽略 inst.rd/inst.funct7)\n\n");

    /* sw  x1, 0(x2) → imm=0 */
    TEST_FULL("sw   x1, 0(x2)",     0x00112023,  FMT_S, OP_STORE, 0);

    /* sw  x3, 16(x4) → imm=16=0x010, rd=imm[4:0]=16, f7=imm[11:5]=0 */
    TEST_FULL("sw   x3, 16(x4)",    0x00322823,  FMT_S, OP_STORE, 16);

    /* sw  x5, -4(x6) → imm=-4=0xFFC, rd=imm[4:0]=0x1C=28, f7=imm[11:5]=0x7F=127 */
    TEST_FULL("sw   x5, -4(x6)",    0xFE532E23,  FMT_S, OP_STORE, -4);

    /* sh  x7, 2(x8) → imm=2, rd=2, f7=0 */
    TEST_FULL("sh   x7, 2(x8)",     0x00741123,  FMT_S, OP_STORE, 2);

    /* sb  x9, 2047(x10) → imm=2047=0x7FF, rd=0x1F=31, f7=0x3F=63 */
    TEST_FULL("sb   x9, 2047(x10)", 0x7E950FA3,  FMT_S, OP_STORE, 2047);

    /* ==================== B-type ==================== */
    printf("\n── B-type (branch) ──\n");
    printf("   Format: imm[12|10:5][31|30:25] | rs2[24:20] | rs1[19:15]"
           " | funct3[14:12] | imm[4:1|11][11:8|7] | opcode[6:0]\n");
    printf("   Note: rd 位置 = imm[4:1|11] (exec 用 inst.imm, 忽略 inst.rd)\n\n");

    /* beq x1, x2, +16 → imm=16=0x010, rd=imm[4:1|11]=0x10>>1=8 */
    TEST_FULL("beq  x1, x2, +16",   0x00208863,  FMT_B, OP_BRANCH, 16);

    /* bne x3, x4, -8 → imm=-8=0xFF8, rd=0xFF8>>1&0xF|(0xFF8>>11&1<<4)=0xC|0x10=28 */
    TEST_FULL("bne  x3, x4, -8",    0xFE419CE3,  FMT_B, OP_BRANCH, -8);

    /* blt x5, x6, +256 → imm=256=0x100, rd=0x100>>1&0xF=0 */
    TEST_FULL("blt  x5, x6, +256",  0x1062C063,  FMT_B, OP_BRANCH, 256);

    /* bge x7, x8, +4 → imm=4, rd=4>>1=2 */
    TEST_FULL("bge  x7, x8, +4",    0x0083D263,  FMT_B, OP_BRANCH, 4);

    /* bltu x1, x2, +0 → imm=0, rd=0 */
    TEST_FULL("bltu x1, x2, +0",    0x0020E063,  FMT_B, OP_BRANCH, 0);

    /* ==================== U-type ==================== */
    printf("\n── U-type (upper immediate) ──\n");
    printf("   Format: imm[31:12][31:12] | rd[11:7] | opcode[6:0]\n");
    printf("   Note: rs1/rs2/funct3 位置 = imm 的高位片段"
           " (exec 用 inst.imm, 忽略 inst.rs1/inst.rs2/inst.funct3)\n\n");

    /* lui  x1, 0x12345 → imm=0x12345000 */
    TEST_FULL("lui  x1, 0x12345",   0x123450B7,  FMT_U, OP_LUI,   0x12345000);

    /* lui  x31, 0xFFFFF → imm=0xFFFFF000, rd=x31=31 */
    TEST_FULL("lui  x31, 0xFFFFF",  0xFFFFFFB7,  FMT_U, OP_LUI,   (s32)0xFFFFF000);

    /* auipc x5, 0x1 → imm=0x1000 */
    TEST_FULL("auipc x5, 0x1",      0x00001297,  FMT_U, OP_AUIPC, 0x00001000);

    /* ==================== J-type ==================== */
    printf("\n── J-type (jump) ──\n");
    printf("   Format: imm[20|10:1|11|19:12][31|30:21|20|19:12]"
           " | rd[11:7] | opcode[6:0]\n");
    printf("   Note: rs1/rs2/funct3 位置 = imm 的分散位\n\n");

    /* jal  x1, +256 → imm=256=0x100 */
    TEST_FULL("jal  x1, +256",      0x100000EF,  FMT_J, OP_JAL,   256);

    /* jal  x0, -4 → imm=-4=0xFFFFC (20-bit sext), raw=0xFFDFF06F */
    TEST_FULL("jal  x0, -4",        0xFFDFF06F,  FMT_J, OP_JAL,   -4);

    /* jal  x5, -1M → imm=-0x100000 (21-bit signed max negative), raw=0x800002EF */
    TEST_FULL("jal  x5, -1M",       0x800002EF,  FMT_J, OP_JAL,   -0x100000);

    /* ==================== JALR (I-type) ==================== */
    printf("\n── JALR (I-type jump register) ──\n\n");

    /* jalr x1, 0(x2) → op=0x67 f3=0 rd=1 rs1=2 imm=0 */
    TEST_FULL("jalr x1, 0(x2)",     0x000100E7,  FMT_I, OP_JALR,  0);

    /* jalr x5, 16(x6) → op=0x67 f3=0 rd=5 rs1=6 imm=16 */
    TEST_FULL("jalr x5, 16(x6)",    0x010302E7,  FMT_I, OP_JALR,  16);

    /* ==================== SYSTEM ==================== */
    printf("\n── SYSTEM (privileged) ──\n\n");

    /* ecall → f3=0 imm=0 */
    TEST_FULL("ecall",              0x00000073,  FMT_I, OP_SYSTEM, 0);

    /* ebreak → f3=0 imm=1 */
    TEST_FULL("ebreak",             0x00100073,  FMT_I, OP_SYSTEM, 1);

    /* mret → f3=0 imm=0x302 */
    TEST_FULL("mret",               0x30200073,  FMT_I, OP_SYSTEM, 0x302);

    /* ==================== 边界测试 ==================== */
    printf("\n── Edge cases ──\n\n");

    /* all-zero instruction (opcode=0, illegal) */
    {
        decoded_inst_t inst;
        bool ok = decode(0x00000000, &inst);
        printf("  [INFO] 0x00000000 → decode=%s (fmt=%d=FMT_UNKNOWN)\n",
               ok ? "true" : "false", inst.fmt);
    }

    /* all-ones instruction (opcode=0x7F, undefined) */
    {
        decoded_inst_t inst;
        bool ok = decode(0xFFFFFFFF, &inst);
        printf("  [INFO] 0xFFFFFFFF → decode=%s (fmt=%d=FMT_UNKNOWN)\n",
               ok ? "true" : "false", inst.fmt);
    }

    /* ==================== 结果汇总 ==================== */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Results: %2d passed, %2d failed                              ║\n",
           passed, failed);
    printf("╚══════════════════════════════════════════════════════════╝\n");
}
