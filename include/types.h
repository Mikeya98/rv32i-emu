/**
 * @file    types.h
 * @brief   RV32I Emulator — 基础类型定义
 */

#ifndef _RVEMU_TYPES_H
#define _RVEMU_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int32_t   s32;
typedef int64_t   s64;

#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

#endif /* _RVEMU_TYPES_H */
