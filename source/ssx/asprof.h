/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_ASPROF_H
#define SSX_ASPROF_H

#include <stdint.h>

#include <psp2/types.h>

// Times ActionScript functions on the logic thread (debug builds, from so_patch)
void ssx_asprof_patch(void);
// The logic thread, once known (perf.c)
void asprof_set_thread(SceUID uid);
// At each logic frame start: logs the previous frame's top functions if report, then starts over
void asprof_frame(uint32_t prev_frame_ms, const char *ui, int report);

#endif // SSX_ASPROF_H
