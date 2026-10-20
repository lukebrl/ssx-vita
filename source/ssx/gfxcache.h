/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_GFXCACHE_H
#define SSX_GFXCACHE_H

#include <stdint.h>

// Hooks UX::GFxHeap's Alloc/Free/Realloc/GetUsableSize/Destroy (from so_patch)
void ssx_gfxcache_patch(void);
// Reuse counts since the last call (watchdog)
void ssx_gfxcache_report(void);

// Debug: the logic thread (call on it), and per logic frame allocation costs (perf.c)
void gfxcache_set_thread(void);
void gfxcache_frame(uint32_t prev_frame_ms, const char *ui, int report);

#endif // SSX_GFXCACHE_H
