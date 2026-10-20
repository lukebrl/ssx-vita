/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_TEXLOAD_H
#define SSX_TEXLOAD_H

// Replaces EA::CoreRender::TranslatePvr/TranslateDds (from so_patch)
void ssx_texload_patch(void);
// Textures loaded since the last call (watchdog)
void ssx_texload_report(void);

#endif // SSX_TEXLOAD_H
