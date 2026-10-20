/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_FASTMATH_H
#define SSX_FASTMATH_H

#include <stdint.h>

// Exact replacements the game's sqrtf and floorf imports resolve to
float ssx_sqrtf(float x);
float ssx_floorf(float x);

// Debug: counted versions of the float math imports
enum { MATH_SQRTF, MATH_FLOORF, MATH_SINF, MATH_COSF, MATH_POWF, MATH_ATAN2F, MATH_ACOSF, MATH_EXPF, MATH_KINDS };
#ifdef DEBUG_SOLOADER
extern volatile uint32_t ssx_math_calls[MATH_KINDS];
float ssx_count_sqrtf(float x);
float ssx_count_floorf(float x);
float ssx_count_sinf(float x);
float ssx_count_cosf(float x);
float ssx_count_powf(float x, float y);
float ssx_count_atan2f(float y, float x);
float ssx_count_acosf(float x);
float ssx_count_expf(float x);
#endif

#endif // SSX_FASTMATH_H
