/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_ALLOCPROF_H
#define SSX_ALLOCPROF_H

#include <stdint.h>

// Times MemoryFramework::Category::Alloc/Free by caller (debug builds, from so_patch)
void ssx_allocprof_patch(void);
void ssx_allocprof_report(uint32_t frames);

#endif // SSX_ALLOCPROF_H
