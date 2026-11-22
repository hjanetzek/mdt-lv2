/*
 * stm32f7xx_hal.h - stands in for the STM32 HAL in the LXR sound engine:
 * integer types and the Cortex-M DSP intrinsics it uses (__SSAT, __QADD16,
 * ...), as instructions on ARMv7E-M (Teensy 4) and in C elsewhere.
 * Not part of the LXR firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_SHIM_STM32F7XX_HAL_H_
#define LXR_SHIM_STM32F7XX_HAL_H_

#include <stdint.h>
#include <stddef.h>

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP

#define __SSAT(ARG1, ARG2) \
	__extension__ ({ int32_t __r, __a = (ARG1); \
	   __asm__ ("ssat %0, %1, %2" : "=r" (__r) : "I" (ARG2), "r" (__a)); __r; })

static inline uint32_t __QADD16 (uint32_t a, uint32_t b)
{
	uint32_t r;
	__asm__ ("qadd16 %0, %1, %2" : "=r" (r) : "r" (a), "r" (b));
	return r;
}

static inline uint32_t __QSUB16 (uint32_t a, uint32_t b)
{
	uint32_t r;
	__asm__ ("qsub16 %0, %1, %2" : "=r" (r) : "r" (a), "r" (b));
	return r;
}

static inline uint64_t __SMLALD (uint32_t a, uint32_t b, uint64_t acc)
{
	union { uint32_t w32[2]; uint64_t w64; } r;
	r.w64 = acc;
	__asm__ ("smlald %0, %1, %2, %3" : "=r" (r.w32[0]), "=r" (r.w32[1]) : "r" (a), "r" (b), "0" (r.w32[0]), "1" (r.w32[1]));
	return r.w64;
}

#else

static inline int32_t lxr_ssat (int32_t x, unsigned bits)
{
	const int32_t max = (1 << (bits - 1)) - 1;
	const int32_t min = -max - 1;
	return x > max ? max : x < min ? min : x;
}
#define __SSAT(ARG1, ARG2) lxr_ssat ((int32_t) (ARG1), (ARG2))

static inline uint32_t __QADD16 (uint32_t a, uint32_t b)
{
	int32_t lo = lxr_ssat ((int16_t) a + (int16_t) b, 16);
	int32_t hi = lxr_ssat ((int16_t) (a >> 16) + (int16_t) (b >> 16), 16);
	return ((uint32_t) hi << 16) | ((uint32_t) lo & 0xFFFF);
}

static inline uint32_t __QSUB16 (uint32_t a, uint32_t b)
{
	int32_t lo = lxr_ssat ((int16_t) a - (int16_t) b, 16);
	int32_t hi = lxr_ssat ((int16_t) (a >> 16) - (int16_t) (b >> 16), 16);
	return ((uint32_t) hi << 16) | ((uint32_t) lo & 0xFFFF);
}

static inline uint64_t __SMLALD (uint32_t a, uint32_t b, uint64_t acc)
{
	return acc + (int64_t) ((int16_t) a * (int16_t) b) + (int64_t) ((int16_t) (a >> 16) * (int16_t) (b >> 16));
}

#endif

// pack two halfwords: the bottom one of a, the top one of b shifted
static inline uint32_t __PKHBT (uint32_t a, uint32_t b, unsigned shift)
{
	return (a & 0xFFFF) | ((b << shift) & 0xFFFF0000);
}

// float to uint32_t as the Cortex-M7 converts (VCVT.U32.F32: saturating,
// negative and NaN 0): a plain cast is undefined for negative values (x86
// wraps them); the FM oscillators convert signed products (Oscillator.c)
static inline uint32_t lxr_f2u (float x)
{
	return x > 0.f ? (x < 4294967296.f ? (uint32_t) x : 0xFFFFFFFFu) : 0;
}

static inline uint8_t __CLZ (uint32_t x)
{
	return x ? __builtin_clz (x) : 32;
}

// interrupt masking (VoiceSlot.c, around a voice change): the MDT engine is
// changed under AudioNoInterrupts() (synth_lxr.cpp) already
static inline uint32_t __get_PRIMASK (void) { return 0; }
static inline void __set_PRIMASK (uint32_t primask) { (void) primask; }
static inline void __disable_irq (void) {}

// the hardware random number generator (random.c)
typedef struct { int unused; } RNG_HandleTypeDef;
extern RNG_HandleTypeDef hrng;
uint32_t HAL_RNG_GetRandomNumber (RNG_HandleTypeDef *hrng);

#endif
