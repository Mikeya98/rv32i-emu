/**
 * @file    fpu.h
 * @brief   RV32F 单精度浮点扩展
 *
 * FLEN = 32, 32 个独立的 f 寄存器。指令本身仍然走
 * fetch → decode → execute, 只是执行阶段交给 fpu_execute。
 */

#ifndef _RVEMU_FPU_H
#define _RVEMU_FPU_H

#include "types.h"
#include "emu.h"

/*
 * FLEN=64 之后, D 和 S 共用同一组 64 位 f 寄存器。
 * 单精度值必须以 NaN-boxed 形式存放: 高 32 位全 1, 低 32 位是 float 位模式。
 * 读一个高 32 位不全 1 的寄存器, 按规范当作规范 NaN。
 */
#define FP32_NAN_BOX     0xFFFFFFFF00000000ull
#define FP32_CANON_NAN   0x7FC00000u

static inline u64 fp32_box(u32 bits)
{
    return FP32_NAN_BOX | (u64)bits;
}

static inline bool fp32_is_boxed(u64 v)
{
    return (v & FP32_NAN_BOX) == FP32_NAN_BOX;
}

/* 取单精度操作数: 没 boxed 的寄存器读作规范 NaN */
static inline u32 fp32_unbox(u64 v)
{
    return fp32_is_boxed(v) ? (u32)v : FP32_CANON_NAN;
}

/**
 * @brief  执行一条 OP-FP 指令 (opcode = 0x53)
 * @return false = 编码不支持 (交给非法指令路径)
 */
bool fpu_execute(emulator_t *emu, decoded_inst_t *inst);

/* 自测: 算术 / 比较 / 转换 / 标志位 / CSR */
void fpu_selftest(void);

#endif /* _RVEMU_FPU_H */
