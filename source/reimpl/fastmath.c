/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fastmath.c
 * @brief sqrtf and floorf for the game, exact and without a library call's overhead.
 *
 * libgame.so is built for softfp and calls the C library for these even
 * where the CPU has an instruction: 1864 call sites for sqrtf, 360 for
 * floorf. newlib's versions also set errno and handle cases the game never
 * checks. These give the same results bit for bit:
 *   - sqrtf is the VFP vsqrt.f32 instruction, correctly rounded as IEEE 754
 *     requires (newlib's result too);
 *   - floorf truncates through an integer conversion; it matches newlib's
 *     floorf on all 2^32 inputs (checked on the PC, work/mathtest).
 *
 * Debug builds count the calls to these and to the other float math
 * functions ([MATH] lines, per second), to tell whether faster approximate
 * versions (sinf, cosf, powf...) could matter.
 */

#include "reimpl/fastmath.h"

#include <math.h>
#include <stdint.h>

float ssx_sqrtf(float x) {
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
}

float ssx_floorf(float x) {
    if (!(fabsf(x) < 8388608.0f))
        return x; // 2^23 and over: already integral (also infinities and NaN)
    if (x == 0.0f)
        return x; // keeps the sign of -0
    float t = (float)(int32_t)x; // rounds toward zero
    return t > x ? t - 1.0f : t;
}

#ifdef DEBUG_SOLOADER
volatile uint32_t ssx_math_calls[MATH_KINDS]; // racy increments: an estimate is enough

float ssx_count_sqrtf(float x) {
    ssx_math_calls[MATH_SQRTF]++;
    return ssx_sqrtf(x);
}
float ssx_count_floorf(float x) {
    ssx_math_calls[MATH_FLOORF]++;
    return ssx_floorf(x);
}
float ssx_count_sinf(float x) {
    ssx_math_calls[MATH_SINF]++;
    return sinf(x);
}
float ssx_count_cosf(float x) {
    ssx_math_calls[MATH_COSF]++;
    return cosf(x);
}
float ssx_count_powf(float x, float y) {
    ssx_math_calls[MATH_POWF]++;
    return powf(x, y);
}
float ssx_count_atan2f(float y, float x) {
    ssx_math_calls[MATH_ATAN2F]++;
    return atan2f(y, x);
}
float ssx_count_acosf(float x) {
    ssx_math_calls[MATH_ACOSF]++;
    return acosf(x);
}
float ssx_count_expf(float x) {
    ssx_math_calls[MATH_EXPF]++;
    return expf(x);
}
#endif
