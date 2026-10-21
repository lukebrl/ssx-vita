/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  overlay.h
 * @brief FPS counter and the port's settings menu, drawn over the game.
 */

#ifndef SSX_OVERLAY_H
#define SSX_OVERLAY_H

#include <stdint.h>

// After vitaGL is initialized
void overlay_init(void);
// Once per presented frame, from the render thread
void overlay_frame(void);

// The settings menu (opened and closed with Select + Start)
void overlay_menu_toggle(void);
int overlay_menu_is_open(void);
// Buttons newly pressed (SCE_CTRL_*) while the menu is open
void overlay_menu_input(uint32_t pressed);

#endif // SSX_OVERLAY_H
