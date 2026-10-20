/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  overlay.h
 * @brief FPS counter drawn over the game.
 */

#ifndef SSX_OVERLAY_H
#define SSX_OVERLAY_H

// After vitaGL is initialized
void overlay_init(void);
// Once per presented frame, from the render thread
void overlay_frame(void);
// Shows or hides the counter (hidden at start)
void overlay_toggle(void);

#endif // SSX_OVERLAY_H
