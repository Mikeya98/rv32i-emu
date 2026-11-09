/**
 * @file    fpu.c
 * @brief   RV32F 单精度浮点扩展实现
 *
 * 只实现 F 扩展 (FLEN=32): 算术、符号注入、min/max、比较、整数转换、
 * FMV/FCLASS, 以及 fflags/frm/fcsr 三个 CSR。
 *
 * 舍入和异常标志交给宿主的 <fenv.h>:
 *   - fesetround 改 MXCSR 的舍入位, 随后的 float 运算就按这个模式舍入
 *   - 运算前后 feclearexcept / fetestexcept 把 IEEE 异常映射到 fflags
 * 这样不用手写 round-to-nearest-even 和 subnormal 处理, 语义也更贴近硬件。
 * 唯一的例外是 RMM (ties away): 宿主 fenv 没有这个模式, 这里退化成 RNE。
 */

#include "fpu.h"
#include "decode.h"
#include "csr.h"
#include "regfile.h"
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* =========================== fflags 位 =========================== */
#define FP_NX   0x01u   /* inexact */
#define FP_UF   0x02u   /* underflow */
#define FP_OF   0x04u   /* overflow */
#define FP_DZ   0x08u   /* divide by zero */
#define FP_NV   0x10u   /* invalid operation */

#define FP_CANON_NAN  0x7FC00000u
#define FP_CANON_NAN64 0x7FF8000000000000ull

/* =========================== 位模式 <-> float/double =========================== */

static double bits_to_f64(u64 bits)
{
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static u64 f64_to_bits(double d)
{
    u64 bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static int bits64_is_nan(u64 bits)
{
    return ((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) &&
           (bits & 0x000FFFFFFFFFFFFFull) != 0;
}

static int bits64_is_snan(u64 bits)
{
    return bits64_is_nan(bits) &&
           (bits & 0x0008000000000000ull) == 0;
}

static float bits_to_f32(u32 bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static u32 f32_to_bits(float f)
{
    u32 bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static int bits_is_nan(u32 bits)
{
    return ((bits & 0x7F800000u) == 0x7F800000u) &&
           (bits & 0x007FFFFFu) != 0;
}

static int bits_is_snan(u32 bits)
{
    return bits_is_nan(bits) && (bits & 0x00400000u) == 0;
}

/* =========================== 舍入 / 标志 =========================== */

/*
 * RISC-V 的 rm: 0=RNE 1=RTZ 2=RDN 3=RUP 4=RMM 7=动态(看 frm)
 * 宿主 fenv 只认 4 种, RMM 这里用 RNE 代替。
 */
static int host_rounding(u32 rm, u32 frm)
{
    if (rm == 7u) {
        rm = frm;
    }

    switch (rm) {
    case 0: return FE_TONEAREST;
    case 1: return FE_TOWARDZERO;
    case 2: return FE_DOWNWARD;
    case 3: return FE_UPWARD;
    case 4: return FE_TONEAREST;
    default: return FE_TONEAREST;   /* 保留编码, 按 RNE 处理 */
    }
}

static void fp_collect_flags(emulator_t *emu)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    u32 f = 0;

    if (e & FE_INEXACT)   f |= FP_NX;
    if (e & FE_UNDERFLOW) f |= FP_UF;
    if (e & FE_OVERFLOW)  f |= FP_OF;
    if (e & FE_DIVBYZERO) f |= FP_DZ;
    if (e & FE_INVALID)   f |= FP_NV;

    emu->csr.fflags |= f;
}

/* =========================== 算术 =========================== */

enum {
    FP_ADD,
    FP_SUB,
    FP_MUL,
    FP_DIV,
    FP_SQRT
};

static u32 fp_arith(emulator_t *emu, u32 rm, int op, u32 abits, u32 bbits)
{
    int old_rm = fegetround();
    float result;
    u32 bits;

    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        /* volatile 保证运算真的发生在这里, 不被挪到改舍入模式之前 */
        volatile float a = bits_to_f32(abits);
        volatile float b = bits_to_f32(bbits);

        switch (op) {
        case FP_ADD:  result = a + b;   break;
        case FP_SUB:  result = a - b;   break;
        case FP_MUL:  result = a * b;   break;
        case FP_DIV:  result = a / b;   break;
        default:      result = sqrtf(a); break;
        }
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    bits = f32_to_bits(result);
    if (bits_is_nan(bits)) {
        bits = FP_CANON_NAN;        /* 结果统一成一个安静的规范 NaN */
    }
    return bits;
}

/* =========================== 双精度算术 (RV32D) =========================== */

static u64 fp_arith_d(emulator_t *emu, u32 rm, int op, u64 abits, u64 bbits)
{
    int old_rm = fegetround();
    double result;
    u64 bits;

    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile double a = bits_to_f64(abits);
        volatile double b = bits_to_f64(bbits);

        switch (op) {
        case FP_ADD:  result = a + b;   break;
        case FP_SUB:  result = a - b;   break;
        case FP_MUL:  result = a * b;   break;
        case FP_DIV:  result = a / b;   break;
        default:      result = sqrt(a); break;
        }
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    bits = f64_to_bits(result);
    if (bits64_is_nan(bits)) {
        bits = FP_CANON_NAN64;
    }
    return bits;
}

/* =========================== min / max =========================== */

static u32 fp_minmax(emulator_t *emu, u32 abits, u32 bbits, int is_max)
{
    int a_nan = bits_is_nan(abits);
    int b_nan = bits_is_nan(bbits);

    if (a_nan && b_nan) {
        return FP_CANON_NAN;
    }
    if (a_nan) {
        if (bits_is_snan(abits)) emu->csr.fflags |= FP_NV;
        return bbits;
    }
    if (b_nan) {
        if (bits_is_snan(bbits)) emu->csr.fflags |= FP_NV;
        return abits;
    }

    {
        float a = bits_to_f32(abits);
        float b = bits_to_f32(bbits);

        if (a < b) return abits;
        if (a > b) return bbits;

        /*
         * 相等。只有 +0 / -0 需要特别处理:
         *   min(-0, +0) = -0, max(-0, +0) = +0
         * 其它相等的值位模式本来就一样。
         */
        {
            u32 sign = is_max ? (abits & bbits & 0x80000000u)
                              : ((abits | bbits) & 0x80000000u);
            return sign | (abits & 0x7FFFFFFFu);
        }
    }
}

/* =========================== 比较 =========================== */

static u32 fp_compare(emulator_t *emu, u8 funct3, u32 abits, u32 bbits)
{
    float a, b;

    if (bits_is_nan(abits) || bits_is_nan(bbits)) {
        if (funct3 == 2u) {
            /* FEQ 是 quiet 比较, 只有 sNaN 才置 NV */
            if (bits_is_snan(abits) || bits_is_snan(bbits)) {
                emu->csr.fflags |= FP_NV;
            }
        } else {
            /* FLT / FLE 是 signaling 比较, 任何 NaN 都置 NV */
            emu->csr.fflags |= FP_NV;
        }
        return 0;
    }

    a = bits_to_f32(abits);
    b = bits_to_f32(bbits);

    switch (funct3) {
    case 0:  return (a <= b) ? 1u : 0u;   /* FLE */
    case 1:  return (a <  b) ? 1u : 0u;   /* FLT */
    default: return (a == b) ? 1u : 0u;   /* FEQ */
    }
}

/* =========================== 双精度 min / max / 比较 =========================== */

static u64 fp_minmax_d(emulator_t *emu, u64 abits, u64 bbits, int is_max)
{
    int a_nan = bits64_is_nan(abits);
    int b_nan = bits64_is_nan(bbits);

    if (a_nan && b_nan) {
        return FP_CANON_NAN64;
    }
    if (a_nan) {
        if (bits64_is_snan(abits)) emu->csr.fflags |= FP_NV;
        return bbits;
    }
    if (b_nan) {
        if (bits64_is_snan(bbits)) emu->csr.fflags |= FP_NV;
        return abits;
    }

    {
        double a = bits_to_f64(abits);
        double b = bits_to_f64(bbits);

        if (a < b) return abits;
        if (a > b) return bbits;

        {
            u64 sign = is_max ? (abits & bbits & 0x8000000000000000ull)
                              : ((abits | bbits) & 0x8000000000000000ull);
            return sign | (abits & 0x7FFFFFFFFFFFFFFFull);
        }
    }
}

static u32 fp_compare_d(emulator_t *emu, u8 funct3, u64 abits, u64 bbits)
{
    double a, b;

    if (bits64_is_nan(abits) || bits64_is_nan(bbits)) {
        if (funct3 == 2u) {
            if (bits64_is_snan(abits) || bits64_is_snan(bbits)) {
                emu->csr.fflags |= FP_NV;
            }
        } else {
            emu->csr.fflags |= FP_NV;
        }
        return 0;
    }

    a = bits_to_f64(abits);
    b = bits_to_f64(bbits);

    switch (funct3) {
    case 0:  return (a <= b) ? 1u : 0u;   /* FLE.D */
    case 1:  return (a <  b) ? 1u : 0u;   /* FLT.D */
    default: return (a == b) ? 1u : 0u;   /* FEQ.D */
    }
}

/* =========================== 转换 =========================== */

/*
 * FCVT.W.S / FCVT.WU.S
 *
 * NaN 和越界都置 NV, 结果按规范饱和到边界值。注意不能直接把越界的
 * float 强转成 int —— C 里那是未定义行为, 必须先判范围。
 */
static u32 fp_float_to_int(emulator_t *emu, u32 rm, u32 abits, int is_unsigned)
{
    float a = bits_to_f32(abits);
    double d = (double)a;
    int old_rm;
    float rounded;

    if (bits_is_nan(abits)) {
        emu->csr.fflags |= FP_NV;
        return is_unsigned ? 0xFFFFFFFFu : 0x7FFFFFFFu;
    }

    if (is_unsigned) {
        if (!(d > 0.0)) {
            if (d < 0.0) {
                emu->csr.fflags |= FP_NV;
                return 0;           /* 负数转 unsigned: 0 + NV */
            }
            return 0;               /* +0 / -0 */
        }
        if (d >= 4294967296.0) {
            emu->csr.fflags |= FP_NV;
            return 0xFFFFFFFFu;
        }
    } else {
        if (d >= 2147483648.0) {
            emu->csr.fflags |= FP_NV;
            return 0x7FFFFFFFu;
        }
        if (d < -2147483648.0) {
            emu->csr.fflags |= FP_NV;
            return 0x80000000u;
        }
    }

    old_rm = fegetround();
    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile float va = a;
        rounded = nearbyintf(va);   /* 按当前舍入模式取整, 不置 inexact */
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    /* nearbyint 不报 inexact, 这里自己比一次 */
    if ((double)rounded != d) {
        emu->csr.fflags |= FP_NX;
    }

    return is_unsigned ? (u32)rounded : (u32)(s32)rounded;
}

/* FCVT.S.W / FCVT.S.WU */
static u32 fp_int_to_float(emulator_t *emu, u32 rm, u32 value, int is_unsigned)
{
    int old_rm = fegetround();
    float result;
    u32 bits;

    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile u32 v = value;
        if (is_unsigned) {
            result = (float)v;
        } else {
            result = (float)(s32)v;
        }
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    bits = f32_to_bits(result);
    if (bits_is_nan(bits)) {
        bits = FP_CANON_NAN;
    }
    return bits;
}

/* =========================== 双精度转换 =========================== */

/* FCVT.W.D / FCVT.WU.D */
static u32 fp_double_to_int(emulator_t *emu, u32 rm, u64 abits, int is_unsigned)
{
    double d = bits_to_f64(abits);
    int old_rm;
    double rounded;

    if (bits64_is_nan(abits)) {
        emu->csr.fflags |= FP_NV;
        return is_unsigned ? 0xFFFFFFFFu : 0x7FFFFFFFu;
    }

    if (is_unsigned) {
        if (!(d > 0.0)) {
            if (d < 0.0) {
                emu->csr.fflags |= FP_NV;
                return 0;
            }
            return 0;
        }
        if (d >= 4294967296.0) {
            emu->csr.fflags |= FP_NV;
            return 0xFFFFFFFFu;
        }
    } else {
        if (d >= 2147483648.0) {
            emu->csr.fflags |= FP_NV;
            return 0x7FFFFFFFu;
        }
        if (d < -2147483648.0) {
            emu->csr.fflags |= FP_NV;
            return 0x80000000u;
        }
    }

    old_rm = fegetround();
    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile double vd = d;
        rounded = nearbyint(vd);
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    if (rounded != d) {
        emu->csr.fflags |= FP_NX;
    }

    return is_unsigned ? (u32)rounded : (u32)(s32)rounded;
}

/* FCVT.D.W / FCVT.D.WU */
static u64 fp_int_to_double(emulator_t *emu, u32 rm, u32 value, int is_unsigned)
{
    int old_rm = fegetround();
    double result;
    u64 bits;

    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile u32 v = value;
        if (is_unsigned) {
            result = (double)v;
        } else {
            result = (double)(s32)v;
        }
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    bits = f64_to_bits(result);
    if (bits64_is_nan(bits)) {
        bits = FP_CANON_NAN64;
    }
    return bits;
}

/* FCVT.D.S: float → double, 精确转换 */
static u64 fp_f32_to_f64(emulator_t *emu, u32 rm, u32 abits)
{
    int old_rm = fegetround();
    double result;
    u64 bits;

    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile float a = bits_to_f32(abits);
        volatile double d = (double)a;
        result = d;
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    bits = f64_to_bits(result);
    if (bits64_is_nan(bits)) {
        bits = FP_CANON_NAN64;
    }
    return bits;
}

/* FCVT.S.D: double → float, 结果按单精度 NaN-box 存 */
static u64 fp_f64_to_f32(emulator_t *emu, u32 rm, u64 abits)
{
    int old_rm = fegetround();
    float result;
    u32 bits;

    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rounding(rm, emu->csr.frm));

    {
        volatile double a = bits_to_f64(abits);
        volatile float f = (float)a;
        result = f;
    }

    fesetround(old_rm);
    fp_collect_flags(emu);

    bits = f32_to_bits(result);
    if (bits_is_nan(bits)) {
        bits = FP_CANON_NAN;
    }
    return fp32_box(bits);
}

/* FCLASS: 10 个分类位 */
static u32 fp_classify(u32 bits)
{
    u32 sign = bits >> 31;
    u32 exp  = (bits >> 23) & 0xFFu;
    u32 frac = bits & 0x007FFFFFu;

    if (exp == 0xFFu) {
        if (frac == 0) {
            return sign ? (1u << 0) : (1u << 7);      /* -inf / +inf */
        }
        return (frac & 0x00400000u) ? (1u << 9) : (1u << 8); /* qNaN / sNaN */
    }
    if (exp == 0) {
        if (frac == 0) {
            return sign ? (1u << 3) : (1u << 4);      /* -0 / +0 */
        }
        return sign ? (1u << 2) : (1u << 5);          /* subnormal */
    }
    return sign ? (1u << 1) : (1u << 6);              /* normal */
}

/* FCLASS.D */
static u32 fp_classify64(u64 bits)
{
    u64 sign = bits >> 63;
    u64 exp  = (bits >> 52) & 0x7FFull;
    u64 frac = bits & 0x000FFFFFFFFFFFFFull;

    if (exp == 0x7FFull) {
        if (frac == 0) {
            return sign ? (1u << 0) : (1u << 7);
        }
        return (frac & 0x0008000000000000ull) ? (1u << 9) : (1u << 8);
    }
    if (exp == 0) {
        if (frac == 0) {
            return sign ? (1u << 3) : (1u << 4);
        }
        return sign ? (1u << 2) : (1u << 5);
    }
    return sign ? (1u << 1) : (1u << 6);
}

/* =========================== 执行 =========================== */

bool fpu_execute(emulator_t *emu, decoded_inst_t *inst)
{
    u8 f3   = inst->funct3;
    u8 f7   = inst->funct7;
    u8 rd   = inst->rd;
    u8 rs1  = inst->rs1;
    u8 rs2  = inst->rs2;
    u64 ra  = emu->rf.fregs[rs1];
    u64 rb  = emu->rf.fregs[rs2];
    u32 a   = fp32_unbox(ra);       /* 单精度操作数, 未 box 的读作规范 NaN */
    u32 b   = fp32_unbox(rb);
    u32 out;

    switch (f7) {
    /* ---------------- 单精度算术 (S) ---------------- */
    case 0x00:      /* FADD.S */
        emu->rf.fregs[rd] = fp32_box(fp_arith(emu, f3, FP_ADD, a, b));
        return true;
    case 0x04:      /* FSUB.S */
        emu->rf.fregs[rd] = fp32_box(fp_arith(emu, f3, FP_SUB, a, b));
        return true;
    case 0x08:      /* FMUL.S */
        emu->rf.fregs[rd] = fp32_box(fp_arith(emu, f3, FP_MUL, a, b));
        return true;
    case 0x0C:      /* FDIV.S */
        emu->rf.fregs[rd] = fp32_box(fp_arith(emu, f3, FP_DIV, a, b));
        return true;
    case 0x2C:      /* FSQRT.S, rs2 必须为 0 */
        if (rs2 != 0u) return false;
        emu->rf.fregs[rd] = fp32_box(fp_arith(emu, f3, FP_SQRT, a, 0));
        return true;

    /* ---------------- 双精度算术 (D) ---------------- */
    case 0x01:      /* FADD.D */
        emu->rf.fregs[rd] = fp_arith_d(emu, f3, FP_ADD, ra, rb);
        return true;
    case 0x05:      /* FSUB.D */
        emu->rf.fregs[rd] = fp_arith_d(emu, f3, FP_SUB, ra, rb);
        return true;
    case 0x09:      /* FMUL.D */
        emu->rf.fregs[rd] = fp_arith_d(emu, f3, FP_MUL, ra, rb);
        return true;
    case 0x0D:      /* FDIV.D */
        emu->rf.fregs[rd] = fp_arith_d(emu, f3, FP_DIV, ra, rb);
        return true;
    case 0x2D:      /* FSQRT.D, rs2 必须为 0 */
        if (rs2 != 0u) return false;
        emu->rf.fregs[rd] = fp_arith_d(emu, f3, FP_SQRT, ra, 0);
        return true;

    /* ---------------- 符号注入 ---------------- */
    case 0x10:      /* FSGNJ.S / FSGNJN.S / FSGNJX.S */
        if (f3 == 0u) {
            out = (a & 0x7FFFFFFFu) | (b & 0x80000000u);
        } else if (f3 == 1u) {
            out = (a & 0x7FFFFFFFu) | ((~b) & 0x80000000u);
        } else if (f3 == 2u) {
            out = (a & 0x7FFFFFFFu) | ((a ^ b) & 0x80000000u);
        } else {
            return false;
        }
        emu->rf.fregs[rd] = fp32_box(out);
        return true;

    case 0x11:      /* FSGNJ.D / FSGNJN.D / FSGNJX.D */
        if (f3 == 0u) {
            emu->rf.fregs[rd] = (ra & 0x7FFFFFFFFFFFFFFFull) |
                                (rb & 0x8000000000000000ull);
        } else if (f3 == 1u) {
            emu->rf.fregs[rd] = (ra & 0x7FFFFFFFFFFFFFFFull) |
                                ((~rb) & 0x8000000000000000ull);
        } else if (f3 == 2u) {
            emu->rf.fregs[rd] = (ra & 0x7FFFFFFFFFFFFFFFull) |
                                ((ra ^ rb) & 0x8000000000000000ull);
        } else {
            return false;
        }
        return true;

    /* ---------------- min / max ---------------- */
    case 0x14:      /* FMIN.S / FMAX.S */
        if (f3 > 1u) return false;
        emu->rf.fregs[rd] = fp32_box(fp_minmax(emu, a, b, f3 == 1u));
        return true;

    case 0x15:      /* FMIN.D / FMAX.D */
        if (f3 > 1u) return false;
        emu->rf.fregs[rd] = fp_minmax_d(emu, ra, rb, f3 == 1u);
        return true;

    /* ---------------- 比较 ---------------- */
    case 0x50:      /* FLE.S / FLT.S / FEQ.S */
        if (f3 > 2u) return false;
        reg_write(&emu->rf, rd, fp_compare(emu, f3, a, b));
        return true;

    case 0x51:      /* FLE.D / FLT.D / FEQ.D */
        if (f3 > 2u) return false;
        reg_write(&emu->rf, rd, fp_compare_d(emu, f3, ra, rb));
        return true;

    /* ---------------- 浮点 → 整数 ---------------- */
    case 0x60:      /* FCVT.W.S / FCVT.WU.S */
        if (rs2 > 1u) return false;
        reg_write(&emu->rf, rd, fp_float_to_int(emu, f3, a, rs2 == 1u));
        return true;

    case 0x61:      /* FCVT.W.D / FCVT.WU.D */
        if (rs2 > 1u) return false;
        reg_write(&emu->rf, rd, fp_double_to_int(emu, f3, ra, rs2 == 1u));
        return true;

    /* ---------------- 整数 → 浮点 ---------------- */
    case 0x68:      /* FCVT.S.W / FCVT.S.WU */
        if (rs2 > 1u) return false;
        emu->rf.fregs[rd] = fp32_box(fp_int_to_float(
            emu, f3, reg_read(&emu->rf, rs1), rs2 == 1u));
        return true;

    case 0x69:      /* FCVT.D.W / FCVT.D.WU */
        if (rs2 > 1u) return false;
        emu->rf.fregs[rd] = fp_int_to_double(
            emu, f3, reg_read(&emu->rf, rs1), rs2 == 1u);
        return true;

    /* ---------------- 单精度 <-> 双精度 ---------------- */
    case 0x20:      /* FCVT.S.D: fmt=00(S), rs2=01(D) */
        if (rs2 != 1u) return false;
        emu->rf.fregs[rd] = fp_f64_to_f32(emu, f3, ra);
        return true;

    case 0x21:      /* FCVT.D.S: fmt=01(D), rs2=00(S) */
        if (rs2 != 0u) return false;
        emu->rf.fregs[rd] = fp_f32_to_f64(emu, f3, a);
        return true;

    /* ---------------- 移动 / 分类 ---------------- */
    case 0x70:      /* FMV.X.W / FCLASS.S */
        if (rs2 != 0u) return false;
        if (f3 == 0u) {
            reg_write(&emu->rf, rd, (u32)ra);   /* 原始低 32 位 */
            return true;
        }
        if (f3 == 1u) {
            reg_write(&emu->rf, rd, fp_classify(a));
            return true;
        }
        return false;

    case 0x71:      /* FCLASS.D (RV32D 没有 FMV.X.D) */
        if (rs2 != 0u || f3 != 1u) return false;
        reg_write(&emu->rf, rd, fp_classify64(ra));
        return true;

    case 0x78:      /* FMV.W.X */
        if (rs2 != 0u || f3 != 0u) return false;
        emu->rf.fregs[rd] = fp32_box(reg_read(&emu->rf, rs1));
        return true;

    default:
        return false;
    }
}

/* =========================== 自测 =========================== */

static void mem_write_inst(memory_t *mem, u32 offset, u32 raw)
{
    memory_write32(mem, offset, raw);
}

static u32 enc_fp(u8 f7, u8 rs2, u8 rs1, u8 f3, u8 rd)
{
    return ((u32)(f7 & 0x7F) << 25) | ((u32)(rs2 & 0x1F) << 20) |
           ((u32)(rs1 & 0x1F) << 15) | ((u32)(f3 & 7) << 12) |
           ((u32)(rd & 0x1F) << 7) | 0x53u;
}

static u32 enc_flw(s32 imm, u8 rs1, u8 fd)
{
    return (((u32)imm & 0xFFFu) << 20) | ((u32)(rs1 & 0x1F) << 15) |
           (2u << 12) | ((u32)(fd & 0x1F) << 7) | 0x07u;
}

static u32 enc_fsw(s32 imm, u8 fs2, u8 rs1)
{
    u32 u = (u32)imm;
    return (((u >> 5) & 0x7Fu) << 25) | ((u32)(fs2 & 0x1F) << 20) |
           ((u32)(rs1 & 0x1F) << 15) | (2u << 12) |
           ((u & 0x1Fu) << 7) | 0x27u;
}

static u32 enc_fld(s32 imm, u8 rs1, u8 fd)
{
    return (((u32)imm & 0xFFFu) << 20) | ((u32)(rs1 & 0x1F) << 15) |
           (3u << 12) | ((u32)(fd & 0x1F) << 7) | 0x07u;
}

static u32 enc_fsd(s32 imm, u8 fs2, u8 rs1)
{
    u32 u = (u32)imm;
    return (((u >> 5) & 0x7Fu) << 25) | ((u32)(fs2 & 0x1F) << 20) |
           ((u32)(rs1 & 0x1F) << 15) | (3u << 12) |
           ((u & 0x1Fu) << 7) | 0x27u;
}

#define CHECK(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", name); \
        fail++; \
    } else { \
        printf("  [OK]   %s\n", name); \
        pass++; \
    } \
} while (0)

static u64 sf32(u32 bits)
{
    return fp32_box(bits);
}

void fpu_selftest(void)
{
    emulator_t emu;
    int pass = 0, fail = 0;
    u32 base = 0x0000;
    u32 bits;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║        RV32F/D Self-Test                                 ║\n");
    printf("║        S/D arith + min/max + cvt + flags + NaN box      ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ---------------- FLW / FSW ---------------- */
    printf("── FLW / FSW ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    memory_write32(&emu.mem, 0x100, 0x3FC00000u);     /* 1.5f */

    mem_write_inst(&emu.mem, base + 0,  0x10000093);  /* addi x1, x0, 0x100 */
    mem_write_inst(&emu.mem, base + 4,  enc_flw(0, 1, 1));
    mem_write_inst(&emu.mem, base + 8,  enc_fsw(4, 1, 1));
    mem_write_inst(&emu.mem, base + 12, 0x0040A183);  /* lw x3, 4(x1) */
    mem_write_inst(&emu.mem, base + 16, 0x00000073);  /* ecall */

    emu.rf.pc = base;
    emu_run(&emu, 5);

    CHECK(emu.rf.fregs[1] == sf32(0x3FC00000u), "flw f1 = 1.5f");
    CHECK(reg_read(&emu.rf, 3) == 0x3FC00000u, "fsw wrote 1.5f back to memory");
    emu_free(&emu);

    /* ---------------- FADD / FSUB / FMUL / FDIV / FSQRT ---------------- */
    printf("── arithmetic ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x3FC00000u);   /* 1.5 */
    emu.rf.fregs[2] = sf32(0x40100000u);   /* 2.25 */
    emu.rf.fregs[3] = sf32(0x40A00000u);   /* 5.0 */
    emu.rf.fregs[4] = sf32(0x40000000u);   /* 2.0 */
    emu.rf.fregs[5] = sf32(0x41100000u);   /* 9.0 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x00, 2, 1, 0, 10)); /* fadd f10,f1,f2 */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x04, 2, 3, 0, 11)); /* fsub f11,f3,f2 */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x08, 2, 1, 0, 12)); /* fmul f12,f1,f2 */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x0C, 4, 3, 0, 13)); /* fdiv f13,f3,f4 */
    mem_write_inst(&emu.mem, base + 16, enc_fp(0x2C, 0, 5, 0, 14)); /* fsqrt f14,f5 */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    CHECK(emu.rf.fregs[10] == sf32(0x40700000u), "fadd 1.5+2.25 = 3.75");
    CHECK(emu.rf.fregs[11] == sf32(0x40300000u), "fsub 5.0-2.25 = 2.75");
    CHECK(emu.rf.fregs[12] == sf32(0x40580000u), "fmul 1.5*2.25 = 3.375");
    CHECK(emu.rf.fregs[13] == sf32(0x40200000u), "fdiv 5.0/2.0 = 2.5");
    CHECK(emu.rf.fregs[14] == sf32(0x40400000u), "fsqrt 9.0 = 3.0");
    emu_free(&emu);

    /* ---------------- FSGNJ / FMIN / FMAX ---------------- */
    printf("── sign inject / min / max ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x3F800000u);   /* +1.0 */
    emu.rf.fregs[2] = sf32(0x80000000u);   /* -0.0 */
    emu.rf.fregs[3] = sf32(0x00000000u);   /* +0.0 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x10, 2, 1, 0, 10)); /* fsgnj  */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x10, 2, 1, 1, 11)); /* fsgnjn */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x10, 2, 1, 2, 12)); /* fsgnjx */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x14, 3, 2, 0, 13)); /* fmin -0,+0 */
    mem_write_inst(&emu.mem, base + 16, enc_fp(0x14, 3, 2, 1, 14)); /* fmax -0,+0 */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    CHECK(emu.rf.fregs[10] == sf32(0xBF800000u), "fsgnj  +1.0 <- -0.0 sign = -1.0");
    CHECK(emu.rf.fregs[11] == sf32(0x3F800000u), "fsgnjn +1.0 <- -0.0 sign = +1.0");
    CHECK(emu.rf.fregs[12] == sf32(0xBF800000u), "fsgnjx +1.0 ^ -0.0 sign = -1.0");
    CHECK(emu.rf.fregs[13] == sf32(0x80000000u), "fmin(-0.0, +0.0) = -0.0");
    CHECK(emu.rf.fregs[14] == sf32(0x00000000u), "fmax(-0.0, +0.0) = +0.0");
    emu_free(&emu);

    /* ---------------- 比较 ---------------- */
    printf("── compare ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x3F800000u);   /* 1.0 */
    emu.rf.fregs[2] = sf32(0x40000000u);   /* 2.0 */
    emu.rf.fregs[3] = sf32(0x7FC00000u);   /* qNaN */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x50, 2, 1, 1, 10)); /* flt 1<2 */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x50, 2, 1, 0, 11)); /* fle 1<=2 */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x50, 2, 1, 2, 12)); /* feq 1==2 */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x50, 3, 1, 2, 13)); /* feq qNaN */
    mem_write_inst(&emu.mem, base + 16, enc_fp(0x50, 3, 1, 1, 14)); /* flt qNaN */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    CHECK(reg_read(&emu.rf, 10) == 1, "flt 1.0 < 2.0 = 1");
    CHECK(reg_read(&emu.rf, 11) == 1, "fle 1.0 <= 2.0 = 1");
    CHECK(reg_read(&emu.rf, 12) == 0, "feq 1.0 == 2.0 = 0");
    CHECK(reg_read(&emu.rf, 13) == 0, "feq qNaN = 0");
    CHECK(reg_read(&emu.rf, 14) == 0, "flt qNaN = 0");
    emu_free(&emu);

    /* ---------------- 转换 ---------------- */
    printf("── fcvt ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x40700000u);   /* 3.75 */
    emu.rf.fregs[2] = sf32(0xC0200000u);   /* -2.5 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x60, 0, 1, 0, 10)); /* fcvt.w.s RNE */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x60, 0, 1, 1, 11)); /* fcvt.w.s RTZ */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x60, 0, 2, 0, 12)); /* -2.5 RNE */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x60, 0, 2, 2, 13)); /* -2.5 RDN */
    mem_write_inst(&emu.mem, base + 16, 0x00700093);                /* addi x1, x0, 7 */
    mem_write_inst(&emu.mem, base + 20, enc_fp(0x68, 0, 1, 0, 14)); /* fcvt.s.w f14, x1 */
    mem_write_inst(&emu.mem, base + 24, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 7);

    CHECK(reg_read(&emu.rf, 10) == 4, "fcvt.w.s 3.75 RNE = 4");
    CHECK(reg_read(&emu.rf, 11) == 3, "fcvt.w.s 3.75 RTZ = 3");
    CHECK((s32)reg_read(&emu.rf, 12) == -2, "fcvt.w.s -2.5 RNE = -2");
    CHECK((s32)reg_read(&emu.rf, 13) == -3, "fcvt.w.s -2.5 RDN = -3");
    CHECK(emu.rf.fregs[14] == sf32(0x40E00000u), "fcvt.s.w 7 = 7.0f");
    emu_free(&emu);

    /* ---------------- FMV / FCLASS ---------------- */
    printf("── fmv / fclass ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x40490FDBu);   /* pi */
    emu.rf.fregs[2] = sf32(0x7F800000u);   /* +inf */
    emu.rf.fregs[3] = sf32(0x7FC00000u);   /* qNaN */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x70, 0, 1, 0, 10)); /* fmv.x.w */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x70, 0, 2, 1, 11)); /* fclass +inf */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x70, 0, 3, 1, 12)); /* fclass qNaN */
    mem_write_inst(&emu.mem, base + 12, 0x3F8002B7);                /* lui x5, 0x3F800 */
    mem_write_inst(&emu.mem, base + 16, enc_fp(0x78, 0, 5, 0, 13)); /* fmv.w.x */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    CHECK(reg_read(&emu.rf, 10) == 0x40490FDBu, "fmv.x.w keeps raw bits");
    CHECK(reg_read(&emu.rf, 11) == (1u << 7), "fclass(+inf) = bit 7");
    CHECK(reg_read(&emu.rf, 12) == (1u << 9), "fclass(qNaN) = bit 9");
    CHECK(emu.rf.fregs[13] == sf32(0x3F800000u), "fmv.w.x keeps raw bits");
    emu_free(&emu);

    /* ---------------- RV32D: FLD / FSD ---------------- */
    printf("── RV32D load/store ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    memory_write64(&emu.mem, 0x100, 0x3FF8000000000000ull);   /* 1.5 */

    mem_write_inst(&emu.mem, base + 0,  0x10000093);          /* addi x1, x0, 0x100 */
    mem_write_inst(&emu.mem, base + 4,  enc_fld(0, 1, 1));
    mem_write_inst(&emu.mem, base + 8,  enc_fsd(8, 1, 1));
    mem_write_inst(&emu.mem, base + 12, 0x0080A183);          /* lw x3, 8(x1) */
    mem_write_inst(&emu.mem, base + 16, 0x00C0A203);          /* lw x4, 12(x1) */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    CHECK(emu.rf.fregs[1] == 0x3FF8000000000000ull, "fld f1 = 1.5 (double)");
    CHECK(reg_read(&emu.rf, 3) == 0x00000000u &&
          reg_read(&emu.rf, 4) == 0x3FF80000u, "fsd wrote 64 bits back");
    emu_free(&emu);

    /* ---------------- RV32D: 算术 ---------------- */
    printf("── RV32D arithmetic ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = 0x3FF8000000000000ull;   /* 1.5 */
    emu.rf.fregs[2] = 0x4002000000000000ull;   /* 2.25 */
    emu.rf.fregs[3] = 0x4014000000000000ull;   /* 5.0 */
    emu.rf.fregs[4] = 0x4000000000000000ull;   /* 2.0 */
    emu.rf.fregs[5] = 0x4022000000000000ull;   /* 9.0 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x01, 2, 1, 0, 10)); /* fadd.d */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x05, 2, 3, 0, 11)); /* fsub.d */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x09, 2, 1, 0, 12)); /* fmul.d */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x0D, 4, 3, 0, 13)); /* fdiv.d */
    mem_write_inst(&emu.mem, base + 16, enc_fp(0x2D, 0, 5, 0, 14)); /* fsqrt.d */
    mem_write_inst(&emu.mem, base + 20, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 6);

    CHECK(emu.rf.fregs[10] == 0x400E000000000000ull, "fadd.d 1.5+2.25 = 3.75");
    CHECK(emu.rf.fregs[11] == 0x4006000000000000ull, "fsub.d 5.0-2.25 = 2.75");
    CHECK(emu.rf.fregs[12] == 0x400B000000000000ull, "fmul.d 1.5*2.25 = 3.375");
    CHECK(emu.rf.fregs[13] == 0x4004000000000000ull, "fdiv.d 5.0/2.0 = 2.5");
    CHECK(emu.rf.fregs[14] == 0x4008000000000000ull, "fsqrt.d 9.0 = 3.0");
    emu_free(&emu);

    /* ---------------- RV32D: 符号 / min / max / 比较 ---------------- */
    printf("── RV32D sign / min / max / compare ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = 0x3FF0000000000000ull;   /* +1.0 */
    emu.rf.fregs[2] = 0x8000000000000000ull;   /* -0.0 */
    emu.rf.fregs[3] = 0x0000000000000000ull;   /* +0.0 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x11, 2, 1, 0, 10)); /* fsgnj.d */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x15, 3, 2, 0, 11)); /* fmin.d */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x15, 3, 2, 1, 12)); /* fmax.d */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x51, 2, 1, 1, 5));  /* flt.d x5 */
    mem_write_inst(&emu.mem, base + 16, enc_fp(0x51, 3, 2, 0, 6));  /* fle.d x6 */
    mem_write_inst(&emu.mem, base + 20, enc_fp(0x51, 3, 2, 2, 7));  /* feq.d x7 */
    mem_write_inst(&emu.mem, base + 24, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 7);

    CHECK(emu.rf.fregs[10] == 0xBFF0000000000000ull, "fsgnj.d +1.0 <- -0.0 = -1.0");
    CHECK(emu.rf.fregs[11] == 0x8000000000000000ull, "fmin.d(-0.0, +0.0) = -0.0");
    CHECK(emu.rf.fregs[12] == 0x0000000000000000ull, "fmax.d(-0.0, +0.0) = +0.0");
    CHECK(reg_read(&emu.rf, 5) == 0, "flt.d 1.0 < -0.0 = 0");
    CHECK(reg_read(&emu.rf, 6) == 1, "fle.d -0.0 <= +0.0 = 1");
    CHECK(reg_read(&emu.rf, 7) == 1, "feq.d -0.0 == +0.0 = 1");
    emu_free(&emu);

    /* ---------------- RV32D: 转换 ---------------- */
    printf("── RV32D fcvt ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = 0x400E000000000000ull;   /* 3.75 */
    emu.rf.fregs[2] = 0xC004000000000000ull;   /* -2.5 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x61, 0, 1, 0, 10)); /* fcvt.w.d RNE */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x61, 0, 1, 1, 11)); /* fcvt.w.d RTZ */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x61, 0, 2, 0, 12)); /* -2.5 RNE */
    mem_write_inst(&emu.mem, base + 12, enc_fp(0x61, 0, 2, 2, 13)); /* -2.5 RDN */
    mem_write_inst(&emu.mem, base + 16, 0x00700093);                /* addi x1, x0, 7 */
    mem_write_inst(&emu.mem, base + 20, enc_fp(0x69, 0, 1, 0, 14)); /* fcvt.d.w f14 */
    mem_write_inst(&emu.mem, base + 24, enc_fp(0x20, 1, 1, 0, 15)); /* fcvt.s.d f15 */
    mem_write_inst(&emu.mem, base + 28, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 8);

    CHECK(reg_read(&emu.rf, 10) == 4, "fcvt.w.d 3.75 RNE = 4");
    CHECK(reg_read(&emu.rf, 11) == 3, "fcvt.w.d 3.75 RTZ = 3");
    CHECK((s32)reg_read(&emu.rf, 12) == -2, "fcvt.w.d -2.5 RNE = -2");
    CHECK((s32)reg_read(&emu.rf, 13) == -3, "fcvt.w.d -2.5 RDN = -3");
    CHECK(emu.rf.fregs[14] == 0x401C000000000000ull, "fcvt.d.w 7 = 7.0");
    CHECK(emu.rf.fregs[15] == sf32(0x40700000u), "fcvt.s.d 3.75 -> boxed 3.75f");
    emu_free(&emu);

    /* ---------------- RV32D: FCVT.D.S / FCLASS.D ---------------- */
    printf("── RV32D widen / fclass ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x3FC00000u);       /* 1.5f */
    emu.rf.fregs[2] = 0x7FF0000000000000ull;   /* +inf (double) */
    emu.rf.fregs[3] = 0x7FF8000000000000ull;   /* qNaN (double) */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x21, 0, 1, 0, 10)); /* fcvt.d.s */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x71, 0, 2, 1, 11)); /* fclass.d +inf */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x71, 0, 3, 1, 12)); /* fclass.d qNaN */
    mem_write_inst(&emu.mem, base + 12, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 4);

    CHECK(emu.rf.fregs[10] == 0x3FF8000000000000ull, "fcvt.d.s 1.5f = 1.5");
    CHECK(reg_read(&emu.rf, 11) == (1u << 7), "fclass.d(+inf) = bit 7");
    CHECK(reg_read(&emu.rf, 12) == (1u << 9), "fclass.d(qNaN) = bit 9");
    emu_free(&emu);

    /* ---------------- NaN boxing ---------------- */
    printf("── NaN boxing ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = 0x3F800000u;             /* 未 box 的 1.0f */
    emu.rf.fregs[2] = sf32(0x3F800000u);       /* 正确 box 的 1.0f */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x00, 2, 1, 0, 10)); /* fadd.s */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x00, 2, 2, 0, 11)); /* fadd.s */
    mem_write_inst(&emu.mem, base + 8,  0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 3);

    CHECK(emu.rf.fregs[10] == sf32(0x7FC00000u),
          "unboxed operand reads as canonical NaN");
    CHECK(emu.rf.fregs[11] == sf32(0x40000000u),
          "boxed operands add normally");
    emu_free(&emu);

    /* ---------------- fflags ---------------- */
    printf("── fflags ──\n");
    emu_init(&emu, MEM_SIZE);
    emu.state = EMU_RUNNING;

    emu.rf.fregs[1] = sf32(0x3F800000u);   /* 1.0 */
    emu.rf.fregs[2] = sf32(0x00000000u);   /* 0.0 */
    emu.rf.fregs[3] = sf32(0x40400000u);   /* 3.0 */

    mem_write_inst(&emu.mem, base + 0,  enc_fp(0x0C, 2, 1, 0, 10)); /* fdiv 1/0 */
    mem_write_inst(&emu.mem, base + 4,  enc_fp(0x0C, 2, 2, 0, 11)); /* fdiv 0/0 */
    mem_write_inst(&emu.mem, base + 8,  enc_fp(0x0C, 3, 1, 0, 12)); /* fdiv 1/3 */
    mem_write_inst(&emu.mem, base + 12, 0x00000073);

    emu.rf.pc = base;
    emu_run(&emu, 4);

    CHECK((emu.csr.fflags & FP_DZ) != 0, "1.0/0.0 sets DZ");
    CHECK((emu.csr.fflags & FP_NV) != 0, "0.0/0.0 sets NV");
    CHECK((emu.csr.fflags & FP_NX) != 0, "1.0/3.0 sets NX");
    CHECK((emu.csr.fflags & FP_OF) == 0, "no OF");
    emu_free(&emu);

    /* ---------------- fcsr CSR ---------------- */
    printf("── fcsr ──\n");
    emu_init(&emu, MEM_SIZE);
    {
        bool ok = false;

        csr_write(&emu.csr, CSR_FCSR, 0x63u, &ok);
        bits = csr_read(&emu.csr, CSR_FCSR, &ok);
        CHECK(ok && bits == 0x63u, "fcsr write/read 0x63");
        CHECK(emu.csr.frm == 3u && emu.csr.fflags == 3u,
              "fcsr split into frm=3 fflags=3");
        csr_write(&emu.csr, CSR_FRM, 0x7u, &ok);
        CHECK(csr_read(&emu.csr, CSR_FRM, &ok) == 7u, "frm accepts dynamic mode");
    }
    emu_free(&emu);

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  FPU Self-Test: %d passed, %d failed%*s║\n",
           pass, fail,
           (int)(38 - snprintf(NULL, 0, "%d", pass) -
                 snprintf(NULL, 0, "%d", fail)), "");
    printf("╚══════════════════════════════════════════════════════════╝\n");
}
