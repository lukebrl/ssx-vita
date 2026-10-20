/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_TERRAINMERGE_H
#define SSX_TERRAINMERGE_H

#include <stdint.h>

// Replaces TerrainRenderObj::DispatchDeferredDispatchBin (from so_patch)
void ssx_terrainmerge_patch(void);
// Chunks and draws per frame since the last call (watchdog)
void ssx_terrainmerge_report(uint32_t frames);

#endif // SSX_TERRAINMERGE_H
