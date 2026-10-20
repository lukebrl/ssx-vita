/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  renderprof.h
 * @brief Logs which code holds the most memory in the "Render" heaps.
 */

#ifndef SSX_RENDERPROF_H
#define SSX_RENDERPROF_H

// Debug aid, off by default: it slows every allocation in RENDER_GEN down a lot
// once its block table has seen many frees (the game ran at 0.1 fps)
#ifndef SSX_RENDERPROF
#define SSX_RENDERPROF 0
#endif

// Call once the memory framework is initialized (from GameRender::PlatInit)
int ssx_renderprof_start(void);
void ssx_renderprof_report(const char *when);

#endif // SSX_RENDERPROF_H
